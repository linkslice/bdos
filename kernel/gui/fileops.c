/*
 * fileops.c - copying, moving and deleting whole trees, for File Manager,
 * drag and drop, and Program Manager.
 */
#include "wm_int.h"

struct names {
	char (*v)[VFS_NAME_MAX];
	int n, cap;
};

static int collect(void *arg, struct vstat *st)
{
	struct names *ns = arg;
	if (ns->n == ns->cap) {
		int nc = ns->cap ? ns->cap * 2 : 32;
		void *nv = krealloc(ns->v, sizeof *ns->v * (size_t)nc);
		if (!nv)
			return 1;
		ns->v = nv;
		ns->cap = nc;
	}
	strncpy0(ns->v[ns->n++], st->name, VFS_NAME_MAX);
	return 0;
}

static int copy_file(const char *src, const char *dst)
{
	int err = 0;
	struct vfile *in = vfs_open(src, O_READ, &err);
	if (!in)
		return err;
	struct vfile *out = vfs_open(dst, O_WRITE | O_CREATE | O_TRUNC, &err);
	if (!out) {
		vfs_close(in);
		return err;
	}
	uint8_t *buf = kmalloc(32 * 1024);
	int r = buf ? 0 : E_NOMEM;
	while (!r) {
		long n = vfs_read(in, buf, 32 * 1024);
		if (n < 0)
			r = (int)n;
		if (n <= 0)
			break;
		long w = vfs_write(out, buf, (size_t)n);
		if (w != n)
			r = w < 0 ? (int)w : E_NOSPC;
		task_yield();		/* big copies: let everything else breathe */
	}
	kfree(buf);
	vfs_close(in);
	vfs_close(out);
	struct vstat st;
	if (!r && !vfs_stat(src, &st))
		vfs_setattr(dst, st.attr & (A_RO | A_HIDDEN | A_SYSTEM | A_ARCHIVE));
	return r;
}

static int copy_tree(const char *src, const char *dst)
{
	struct vstat st;
	int r = vfs_stat(src, &st);
	if (r)
		return r;
	if (st.type != VFS_DIR)
		return copy_file(src, dst);
	r = vfs_mkdir(dst);
	if (r && r != E_EXIST)
		return r;
	struct names ns = { NULL, 0, 0 };
	r = vfs_list(src, collect, &ns);
	for (int i = 0; !r && i < ns.n; i++) {
		char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
		if (vfs_join(src, ns.v[i], a, sizeof a) || vfs_join(dst, ns.v[i], b, sizeof b)) {
			r = E_NAMETOOLONG;
			break;
		}
		r = copy_tree(a, b);
	}
	kfree(ns.v);
	return r;
}

int fop_delete(const char *path)
{
	struct vstat st;
	int r = vfs_stat(path, &st);
	if (r)
		return r;
	if (st.attr & A_RO)
		vfs_setattr(path, st.attr & ~A_RO);	/* File Manager already asked */
	if (st.type != VFS_DIR)
		return vfs_delete(path);
	struct names ns = { NULL, 0, 0 };
	r = vfs_list(path, collect, &ns);
	for (int i = 0; !r && i < ns.n; i++) {
		char a[VFS_PATH_MAX];
		if (vfs_join(path, ns.v[i], a, sizeof a)) {
			r = E_NAMETOOLONG;
			break;
		}
		r = fop_delete(a);
	}
	kfree(ns.v);
	return r ? r : vfs_rmdir(path);
}

static int inside(const char *dir, const char *path)
{
	/* is path dir itself, or somewhere under it? */
	size_t n = strlen(dir);
	return !strncasecmp(dir, path, n) && (path[n] == 0 || path[n] == '\\' || (n && dir[n - 1] == '\\'));
}

int fop_exists_in(const char *src, const char *dst_dir)
{
	char dst[VFS_PATH_MAX];
	struct vstat st;
	if (vfs_join(dst_dir, vfs_basename(src), dst, sizeof dst))
		return 0;
	return !vfs_stat(dst, &st) && strcasecmp(dst, src);
}

int fop_copy(const char *src, const char *dst_dir, int flags)
{
	char dst[VFS_PATH_MAX];
	if (vfs_join(dst_dir, vfs_basename(src), dst, sizeof dst))
		return E_NAMETOOLONG;
	if (!strcasecmp(src, dst))
		return (flags & FOP_MOVE) ? 0 : E_EXIST;	/* onto itself */
	if (inside(src, dst_dir))
		return E_INVAL;				/* a folder into itself */
	struct vstat st;
	if (!vfs_stat(dst, &st)) {
		if (!(flags & FOP_REPLACE))
			return E_EXIST;
		int r = fop_delete(dst);
		if (r)
			return r;
	}
	if ((flags & FOP_MOVE) && toupper(src[0]) == toupper(dst[0])) {
		int r = vfs_rename(src, dst);
		if (r != E_XDEV && r != E_NOTSUP)
			return r;
	}
	int r = copy_tree(src, dst);
	if (!r && (flags & FOP_MOVE))
		r = fop_delete(src);
	return r;
}

int fop_unique(const char *dir, const char *name, char *out, size_t n)
{
	struct vstat st;
	if (vfs_join(dir, name, out, n))
		return E_NAMETOOLONG;
	if (vfs_stat(out, &st))
		return 0;
	char base[VFS_NAME_MAX], ext[VFS_NAME_MAX] = "";
	strncpy0(base, name, sizeof base);
	char *dot = strrchr(base, '.');
	if (dot && dot != base) {
		strncpy0(ext, dot, sizeof ext);
		*dot = 0;
	}
	for (int i = 2; i < 100; i++) {
		char cand[VFS_NAME_MAX];
		snprintf(cand, sizeof cand, "%s (%d)%s", base, i, ext);
		if (vfs_join(dir, cand, out, n))
			return E_NAMETOOLONG;
		if (vfs_stat(out, &st))
			return 0;
	}
	return E_EXIST;
}
