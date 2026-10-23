#ifndef _DIRENT_H
#define _DIRENT_H
struct dirent {
	unsigned long d_ino;
	unsigned char d_type;
	char d_name[256];
};
#define DT_DIR 4
#define DT_REG 8
typedef struct DIR DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
#endif
