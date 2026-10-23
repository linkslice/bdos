#ifndef _SETJMP_H
#define _SETJMP_H
typedef unsigned long jmp_buf[8];
int setjmp(jmp_buf jb);
_Noreturn void longjmp(jmp_buf jb, int v);
#define _setjmp setjmp
#define _longjmp longjmp
#endif
