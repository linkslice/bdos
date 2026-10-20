/* CTEST.C - checks the C interpreter; prints ALL OK at the end */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHECK(x) check(x, __LINE__)
enum colour { RED, GREEN = 5, BLUE };
typedef struct { int x, y; } Pt;
struct rec { char tag; double v; Pt p; int arr[3]; };

int fails = 0;
int counter(void) { static int n = 0; return ++n; }
int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }
Pt mkpt(int x, int y) { Pt p; p.x = x; p.y = y; return p; }
int sum(int *a, int n) { int s = 0; while (n--) s += *a++; return s; }
int later(int);

void check(int ok, int line)
{
	if (!ok) {
		printf("FAIL line %d\n", line);
		fails++;
	}
}

int main(void)
{
	int a = 7, b = 3, i, grid[3][4];
	long big = 1L << 40;
	unsigned int u = 0xFFFFFFFF;
	char s[32], *p;
	double d = 1.5;
	struct rec r = { 'q', 2.25, { 1, 2 }, { 4, 5, 6 } };
	Pt q;
	int nums[] = { 5, 1, 4 };

	CHECK(a / b == 2 && a % b == 1 && -a / b == -2);
	CHECK((a & b) == 3 && (a | 8) == 15 && (a ^ b) == 4 && (a << 2) == 28);
	CHECK(big == 1099511627776L);
	CHECK(u + 1 == 0 && u > 0);
	CHECK(d * 2 == 3.0 && (int)(d * 3) == 4);
	CHECK(7 / 2.0 == 3.5);
	CHECK(GREEN == 5 && BLUE == 6 && RED == 0);
	CHECK(fib(15) == 610);
	swap(&a, &b);
	CHECK(a == 3 && b == 7);
	q = mkpt(3, 4);
	CHECK(q.x * q.x + q.y * q.y == 25);
	CHECK(r.tag == 'q' && r.v == 2.25 && r.p.y == 2 && r.arr[2] == 6);
	CHECK(sizeof(struct rec) == 40 && sizeof(Pt) == 8 && sizeof nums == 12);
	CHECK(sum(nums, 3) == 10);
	for (i = 0; i < 12; i++)
		grid[i / 4][i % 4] = i;
	CHECK(grid[2][3] == 11 && *(*(grid + 1) + 2) == 6);
	strcpy(s, "hello");
	strcat(s, ", world");
	CHECK(strlen(s) == 12 && !strcmp(s, "hello, world"));
	p = strchr(s, 'w');
	CHECK(p && p - s == 7 && *p == 'w');
	sprintf(s, "%05d|%-3s|%x|%.2f", 42, "ab", 255, 3.14159);
	CHECK(!strcmp(s, "00042|ab |ff|3.14"));
	CHECK(counter() == 1 && counter() == 2);
	i = 0;
	do i += 2; while (i < 9);
	CHECK(i == 10);
	for (i = 0; i < 10; i++) {
		if (i == 3) continue;
		if (i == 6) break;
	}
	CHECK(i == 6);
	switch (b) {
	case 1: i = 100; break;
	case 7: i = 700;
	case 8: i += 1; break;
	default: i = -1;
	}
	CHECK(i == 701);
	i = 5;
	CHECK(i++ == 5 && ++i == 7 && i-- == 7 && i == 6);
	i += 4; i *= 2; i -= 1; i /= 3; i <<= 1;
	CHECK(i == 12);
	CHECK(later(4) == 16);
	CHECK(atoi("-123") == -123 && abs(-9) == 9);
	CHECK('A' + 1 == 'B' && "xyz"[1] == 'y');
	CHECK(fails == 0 ? 1 : 0);
	if (!fails)
		printf("ALL OK\n");
	return fails;
}

int later(int x) { return x * x; }
