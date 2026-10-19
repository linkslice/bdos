/*
 * mkbdosfs - make a bdosfs volume and fill it from a directory on the host.
 *
 *   mkbdosfs IMAGE OFFSET SIZE LABEL [ROOTDIR]
 *   mkbdosfs --check IMAGE OFFSET
 *   mkbdosfs --ls IMAGE OFFSET [PATH]
 *   mkbdosfs --cat IMAGE OFFSET PATH
 *
 * OFFSET and SIZE are in bytes (the image file is created or extended as
 * needed). In ROOTDIR, an optional ".attrib" file per directory sets DOS
 * attributes ("KERNEL.SYS SHR") and ".xattr" sets extended attributes
 * ("Main icon=prompt").
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../fs/bdosfs.h"

static int fd;
static uint64_t base;

uint64_t bfs_now(void)
{
	return (uint64_t)time(NULL) + (uint64_t)localtime(&(time_t){ time(NULL) })->tm_gmtoff;
}

static int hread(void *ctx, uint64_t blk, uint32_t n, void *buf)
{
	(void)ctx;
	ssize_t r = pread(fd, buf, (size_t)n * BFS_BLOCK, (off_t)(base + blk * BFS_BLOCK));
	if (r < 0)
		return -1;
	if ((size_t)r < (size_t)n * BFS_BLOCK)	/* sparse tail of a new image */
		memset((char *)buf + r, 0, (size_t)n * BFS_BLOCK - (size_t)r);
	return 0;
}

static int hwrite(void *ctx, uint64_t blk, uint32_t n, const void *buf)
{
	(void)ctx;
	return pwrite(fd, buf, (size_t)n * BFS_BLOCK, (off_t)(base + blk * BFS_BLOCK)) ==
	    (ssize_t)((size_t)n * BFS_BLOCK) ? 0 : -1;
}

static int hflush(void *ctx)
{
	(void)ctx;
	return fsync(fd);
}

static void die(const char *what, int err)
{
	fprintf(stderr, "mkbdosfs: %s: %s\n", what, err < 0 ? bfs_strerror(err) : "failed");
	exit(1);
}

/* "name rest" or "\"name with spaces\" rest" */
static int split_meta(const char *line, char *name, size_t nn, char *rest, size_t rn)
{
	const char *p = line;
	size_t o = 0;
	while (*p == ' ')
		p++;
	if (*p == '#' || !*p || *p == '\n')
		return 0;
	if (*p == '"') {
		p++;
		while (*p && *p != '"' && o + 1 < nn)
			name[o++] = *p++;
		if (*p == '"')
			p++;
	} else {
		while (*p && *p != ' ' && *p != '\n' && o + 1 < nn)
			name[o++] = *p++;
	}
	name[o] = 0;
	while (*p == ' ')
		p++;
	o = 0;
	while (*p && *p != '\n' && *p != '\r' && o + 1 < rn)
		rest[o++] = *p++;
	rest[o] = 0;
	return name[0] && rest[0];
}

static void apply_meta(struct bfs *fs, uint32_t dir, const char *hostdir)
{
	char p[4096], line[512];
	snprintf(p, sizeof p, "%s/.attrib", hostdir);
	FILE *f = fopen(p, "r");
	if (f) {
		while (fgets(line, sizeof line, f)) {
			char name[256], attrs[16] = "";
			if (!split_meta(line, name, sizeof name, attrs, sizeof attrs))
				continue;
			uint32_t ino;
			struct bfs_stat st;
			if (bfs_lookup(fs, dir, name, &ino) || bfs_stat(fs, ino, &st)) {
				fprintf(stderr, "mkbdosfs: .attrib: no %s in %s\n", name, hostdir);
				continue;
			}
			uint8_t a = st.attr;
			for (char *c = attrs; *c; c++) {
				if (*c == 'R') a |= BFS_ATTR_RO;
				if (*c == 'H') a |= BFS_ATTR_HIDDEN;
				if (*c == 'S') a |= BFS_ATTR_SYSTEM;
				if (*c == 'A') a |= BFS_ATTR_ARCHIVE;
			}
			bfs_setattr(fs, ino, a);
		}
		fclose(f);
	}
	snprintf(p, sizeof p, "%s/.xattr", hostdir);
	f = fopen(p, "r");
	if (f) {
		while (fgets(line, sizeof line, f)) {
			char name[256], kv[256];
			if (!split_meta(line, name, sizeof name, kv, sizeof kv))
				continue;
			char *eq = strchr(kv, '=');
			uint32_t ino;
			if (!eq || bfs_lookup(fs, dir, name, &ino))
				continue;
			*eq = 0;
			bfs_xattr_set(fs, ino, kv, eq + 1);
		}
		fclose(f);
	}
}

static int files_copied;

static void copy_in(struct bfs *fs, uint32_t dir, const char *hostdir)
{
	DIR *d = opendir(hostdir);
	if (!d) {
		perror(hostdir);
		exit(1);
	}
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;	/* . .. .attrib .xattr */
		char p[4096];
		snprintf(p, sizeof p, "%s/%s", hostdir, de->d_name);
		struct stat st;
		if (stat(p, &st))
			continue;
		uint32_t ino;
		if (S_ISDIR(st.st_mode)) {
			int r = bfs_create(fs, dir, de->d_name, BFS_DIR, &ino);
			if (r)
				die(p, r);
			copy_in(fs, ino, p);
		} else if (S_ISREG(st.st_mode)) {
			int r = bfs_create(fs, dir, de->d_name, BFS_FILE, &ino);
			if (r)
				die(p, r);
			FILE *f = fopen(p, "rb");
			if (!f)
				die(p, 0);
			char *buf = malloc((size_t)st.st_size + 1);
			size_t n = fread(buf, 1, (size_t)st.st_size, f);
			fclose(f);
			long w = n ? bfs_write(fs, ino, 0, buf, n) : 0;
			free(buf);
			if (w < 0)
				die(p, (int)w);
			bfs_touch(fs, ino, (uint64_t)st.st_mtime);
			files_copied++;
		}
	}
	closedir(d);
	apply_meta(fs, dir, hostdir);
}

static void say(const char *m)
{
	printf("  %s\n", m);
}

static int walk(struct bfs *fs, const char *path, uint32_t *ino)
{
	uint32_t cur = BFS_ROOT;
	char buf[1024];
	snprintf(buf, sizeof buf, "%s", path);
	for (char *tok = strtok(buf, "/\\"); tok; tok = strtok(NULL, "/\\"))
		if (bfs_lookup(fs, cur, tok, &cur))
			return -1;
	*ino = cur;
	return 0;
}

int main(int argc, char **argv)
{
	int check = argc > 1 && !strcmp(argv[1], "--check");
	int ls = argc > 1 && !strcmp(argv[1], "--ls");
	int cat = argc > 1 && !strcmp(argv[1], "--cat");
	if (check || ls || cat) {
		if (argc < 4) {
			fprintf(stderr, "usage: mkbdosfs --check|--ls|--cat IMAGE OFFSET [PATH]\n");
			return 2;
		}
		fd = open(argv[2], O_RDWR);
		if (fd < 0) {
			perror(argv[2]);
			return 1;
		}
		base = strtoull(argv[3], NULL, 0);
		struct stat st;
		fstat(fd, &st);
		struct bfs_dev dev = { hread, hwrite, hflush, NULL,
		    ((uint64_t)st.st_size - base) / BFS_BLOCK };
		struct bfs *fs;
		int r = bfs_mount(&dev, &fs);
		if (r)
			die("mount", r);
		if (check) {
			r = bfs_check(fs, 0, say);
			printf("%s\n", r ? "PROBLEMS FOUND" : "clean");
			return r ? 1 : 0;
		}
		uint32_t ino;
		if (walk(fs, argc > 4 ? argv[4] : "", &ino))
			die(argc > 4 ? argv[4] : "/", BFS_ENOENT);
		if (cat) {
			static char buf[1 << 20];
			long n;
			uint64_t off = 0;
			while ((n = bfs_read(fs, ino, off, buf, sizeof buf)) > 0) {
				fwrite(buf, 1, (size_t)n, stdout);
				off += (uint64_t)n;
			}
			return n < 0;
		}
		uint64_t cookie = 0;
		struct bfs_listent e;
		while (bfs_readdir(fs, ino, &cookie, &e) == 1)
			printf("%c %02x %10llu  %-12s %s\n", e.st.type == BFS_DIR ? 'd' : '-',
			    e.st.attr, (unsigned long long)e.st.size, e.st.short_name, e.name);
		return 0;
	}

	if (argc < 5) {
		fprintf(stderr, "usage: mkbdosfs IMAGE OFFSET SIZE LABEL [ROOTDIR]\n");
		return 2;
	}
	fd = open(argv[1], O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	base = strtoull(argv[2], NULL, 0);
	uint64_t size = strtoull(argv[3], NULL, 0);
	struct stat st;
	fstat(fd, &st);
	if ((uint64_t)st.st_size < base + size && ftruncate(fd, (off_t)(base + size))) {
		perror("ftruncate");
		return 1;
	}
	struct bfs_dev dev = { hread, hwrite, hflush, NULL, size / BFS_BLOCK };
	int r = bfs_format(&dev, argv[4], bfs_now());
	if (r)
		die("format", r);
	struct bfs *fs;
	if ((r = bfs_mount(&dev, &fs)))
		die("mount", r);
	if (argc > 5)
		copy_in(fs, BFS_ROOT, argv[5]);
	r = bfs_check(fs, 0, say);
	if (r)
		die("check after copy", BFS_ECORRUPT);
	printf("bdosfs %s: %llu KiB, %d files, %llu KiB free\n", argv[4],
	    (unsigned long long)(size / 1024), files_copied,
	    (unsigned long long)(bfs_free_bytes(fs) / 1024));
	bfs_unmount(fs);
	close(fd);
	return 0;
}
