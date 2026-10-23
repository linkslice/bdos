/* LIBCTEST.EXE - checks the C library (the test suite runs it) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <bdos.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void checkfmt(const char *want, const char *fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (strcmp(b, want)) {
		printf("FAIL format \"%s\": got \"%s\", want \"%s\"\n", fmt, b, want);
		fails++;
	}
}

static jmp_buf jb;
static void jump(int v) { longjmp(jb, v); }

static int cmpint(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static bdos_mutex m = BDOS_MUTEX_INIT;
static long shared;
static int adder(void *arg)
{
	for (int i = 0; i < 100000; i++) {
		bdos_lock(&m);
		shared++;
		bdos_unlock(&m);
	}
	return (int)(long)arg;
}

int main(void)
{
	checkfmt("42", "%d", 42);
	checkfmt("-0042", "%05d", -42);
	checkfmt("  -42", "%5d", -42);
	checkfmt("-42  |", "%-5d|", -42);
	checkfmt("ff FF 0xff 377", "%x %X %#x %o", 255, 255, 255, 255);
	checkfmt("18446744073709551615", "%llu", ~0ull);
	checkfmt("-9223372036854775808", "%lld", (long long)(-9223372036854775807LL - 1));
	checkfmt("3.141593", "%f", 3.14159265358979);
	checkfmt("3.14", "%.2f", 3.14159);
	checkfmt("1.000000e+10", "%e", 1e10);
	checkfmt("0.1", "%g", 0.1);
	checkfmt("1e+20", "%g", 1e20);
	checkfmt("100000", "%g", 100000.0);
	checkfmt("1e+06", "%g", 1000000.0);
	checkfmt("0.0001", "%g", 0.0001);
	checkfmt("1e-05", "%g", 0.00001);
	checkfmt("2", "%.0f", 2.5);
	checkfmt("4", "%.0f", 3.5);
	checkfmt("0.30000000000000004", "%.17g", 0.1 + 0.2);
	checkfmt("1.7976931348623157e+308", "%.17g", 1.7976931348623157e308);
	checkfmt("4.9406564584124654e-324", "%.17g", 4.9406564584124654e-324);
	checkfmt("0x1.8p+1", "%a", 3.0);
	checkfmt("inf -inf nan", "%f %f %f", INFINITY, -INFINITY, NAN);
	checkfmt("hello|   hi|hi   |he", "%s|%5s|%-5s|%.2s", "hello", "hi", "hi", "hello");
	checkfmt("x%y", "x%%y");
	checkfmt("c", "%c", 'c');

	CHECK(strtod("0.1", NULL) == 0.1);
	CHECK(strtod("1e308", NULL) == 1e308);
	CHECK(strtod("2.2250738585072014e-308", NULL) == 2.2250738585072014e-308);
	CHECK(strtod("4.9406564584124654e-324", NULL) == 4.9406564584124654e-324);
	CHECK(strtod("123456789012345678901234567890", NULL) == 0x1.8ee90ff6c373ep+96);
	CHECK(strtod("0x1.8p1", NULL) == 3.0);
	CHECK(strtod("9007199254740993", NULL) == 9007199254740992.0);	/* tie: to even */
	CHECK(strtol("-0x1F", NULL, 0) == -31);
	CHECK(strtoul("777", NULL, 8) == 511);
	CHECK(atoi("  12abc") == 12);

	int a, n;
	double d;
	char w[32];
	CHECK(sscanf("12 3.5 word", "%d %lf %31s%n", &a, &d, w, &n) == 3 && a == 12 && d == 3.5 && !strcmp(w, "word") && n == 11);
	unsigned x;
	CHECK(sscanf("0x1f,abc", "%x,%3[a-c]", &x, w) == 2 && x == 31 && !strcmp(w, "abc"));

	CHECK(fabs(sin(M_PI / 6) - 0.5) < 1e-15);
	CHECK(fabs(cos(0) - 1) < 1e-15);
	CHECK(fabs(sqrt(2) - M_SQRT2) < 1e-15);
	CHECK(fabs(exp(1) - M_E) < 1e-15);
	CHECK(fabs(log(M_E) - 1) < 1e-15);
	CHECK(pow(2, 10) == 1024);
	CHECK(fabs(pow(2, 0.5) - M_SQRT2) < 1e-15);
	CHECK(floor(-1.5) == -2 && ceil(-1.5) == -1 && round(2.5) == 3 && trunc(-2.7) == -2);
	CHECK(fmod(7, 3) == 1);
	CHECK(fabs(atan2(1, 1) - M_PI / 4) < 1e-15);
	CHECK(ldexp(1, -1074) == 4.9406564584124654e-324);

	char s[64];
	strcpy(s, "hello");
	strcat(s, " world");
	CHECK(!strcmp(s, "hello world") && strlen(s) == 11);
	CHECK(strstr(s, "wor") == s + 6 && strrchr(s, 'o') == s + 7);
	CHECK(!strcasecmp("ABC", "abc") && toupper('q') == 'Q');

	int arr[100];
	for (int i = 0; i < 100; i++)
		arr[i] = (i * 37) % 101;
	qsort(arr, 100, sizeof arr[0], cmpint);
	int sorted = 1;
	for (int i = 1; i < 100; i++)
		sorted &= arr[i - 1] <= arr[i];
	CHECK(sorted);

	int r = setjmp(jb);
	if (!r)
		jump(7);
	CHECK(r == 7);

	/* the heap: lots of sizes, then check nothing overlapped */
	char *ps[200];
	for (int i = 0; i < 200; i++) {
		ps[i] = malloc((size_t)(i * 997 % 5000 + 1));
		memset(ps[i], i, (size_t)(i * 997 % 5000 + 1));
	}
	int heap_ok = 1;
	for (int i = 0; i < 200; i++) {
		for (int k = 0; k < i * 997 % 5000 + 1; k++)
			if (ps[i][k] != (char)i)
				heap_ok = 0;
		free(ps[i]);
	}
	char *big = malloc(8 << 20);
	CHECK(big);
	memset(big, 1, 8 << 20);
	free(big);
	CHECK(heap_ok);

	/* files */
	FILE *f = fopen("C:\\LIBCTEST.TXT", "w");
	CHECK(f);
	for (int i = 0; i < 1000; i++)
		fprintf(f, "line %d\n", i);
	fclose(f);
	f = fopen("C:\\LIBCTEST.TXT", "r");
	CHECK(f);
	int lines = 0, last = -1;
	char line[64];
	while (fgets(line, sizeof line, f)) {
		sscanf(line, "line %d", &last);
		lines++;
	}
	CHECK(lines == 1000 && last == 999);
	fseek(f, 0, SEEK_END);
	CHECK(ftell(f) == 8890);
	fclose(f);
	CHECK(!remove("C:\\LIBCTEST.TXT"));
	CHECK(!fopen("C:\\LIBCTEST.TXT", "r") && errno == ENOENT);

	DIR *dir = opendir("C:\\");
	int found = 0;
	struct dirent *de;
	while (dir && (de = readdir(dir)))
		if (!strcmp(de->d_name, "BDOS") && de->d_type == DT_DIR)
			found = 1;
	if (dir)
		closedir(dir);
	CHECK(found);

	/* threads and a mutex */
	int t1 = bdos_thread(adder, (void *)1L), t2 = bdos_thread(adder, (void *)2L), t3 = bdos_thread(adder, (void *)3L);
	CHECK(bdos_join(t1) == 1 && bdos_join(t2) == 2 && bdos_join(t3) == 3);
	CHECK(shared == 300000);

	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	CHECK(tm->tm_year >= 124);
	time_t zero = 0;
	strftime(s, sizeof s, "%Y-%m-%d %H:%M:%S", gmtime(&zero));
	CHECK(!strcmp(s, "1970-01-01 00:00:00"));

	printf("libctest: %s (%d failure%s)\n", fails ? "FAILED" : "all passed", fails, fails == 1 ? "" : "s");
	return fails != 0;
}
