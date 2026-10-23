#ifndef _UNISTD_H
#define _UNISTD_H
#include <sys/types.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
int unlink(const char *path);
int rmdir(const char *path);
int chdir(const char *path);
char *getcwd(char *buf, size_t n);
int access(const char *path, int mode);
void *sbrk(long inc);
unsigned sleep(unsigned s);
int usleep(unsigned long us);
int isatty(int fd);
int dup(int fd);
int dup2(int a, int b);
pid_t getpid(void);
long sysconf(int name);
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PAGESIZE 30
int execvp(const char *file, char *const argv[]);
extern char **environ;
#endif
