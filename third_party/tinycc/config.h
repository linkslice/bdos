/* config.h - tcc built for bdos (written by hand; tcc's configure isn't used) */
#define TCC_VERSION "0.9.28rc"
#define CONFIG_TCC_STATIC 1		/* no dlopen */
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCC_PREDEFS 0		/* read tccdefs.h from the include folder */
#define CONFIG_TCCDIR "C:/BDOS/TCC"
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include;C:/BDOS/INCLUDE"
#define CONFIG_TCC_LIBPATHS "{B};C:/BDOS/LIB"
#define CONFIG_TCC_CRTPREFIX "C:/BDOS/LIB"
#define CONFIG_TCC_ELFINTERP "-"
#define TCC_TARGET_X86_64 1
#define ONE_SOURCE 1
