/*
 * basic.c - BBC-style BASIC for bdos.
 *
 * Numbered or unnumbered programs; reals (A), integers (A%), strings (A$)
 * and arrays; PRINT/INPUT/IF-THEN-ELSE (single line and block)/FOR/REPEAT/
 * WHILE/GOTO/GOSUB/ON; DEF PROC and DEF FN with LOCAL; DATA/READ;
 * SYS "name", args TO result; OSCLI and *commands; GET, INKEY, TIME, RND.
 *
 * Immediate mode (BASIC with no file) has RUN, LIST, NEW, LOAD, SAVE,
 * RENUMBER, CHAIN and QUIT, like the BBC Micro.
 */
#include "../kernel.h"
#include "../vfs.h"
#include "../sysapi.h"
#include "fmath.h"

#define MAX_LINE	256
#define STACK_MAX	256
#define NHASH		64
#define NAME_MAX_B	40

enum { T_NUM, T_STR };

struct val {
	int t;
	double n;
	char *s;		/* owned; NULL means "" */
};

struct var {
	char name[NAME_MAX_B];	/* includes the % or $ suffix, and "(" for arrays */
	int is_str, is_int, is_array;
	double n;
	char *s;
	int ndims, dims[4], count;
	double *na;
	char **sa;
	struct var *next;
};

struct line {
	int num;		/* 0: unnumbered */
	int fileline;		/* for error messages in unnumbered programs */
	char *text;
};

enum { F_GOSUB, F_FOR, F_REPEAT, F_WHILE, F_PROC, F_FN };

struct saved {
	struct var *v;
	double n;
	char *s;
};

struct frame {
	int kind;
	int line;		/* where to come back to */
	const char *pos;
	struct var *v;		/* FOR */
	double limit, step;
	struct saved *saved;	/* PROC/FN locals */
	int nsaved;
	struct val ret;		/* FN result */
};

struct alloc {
	struct alloc *prev, *next;
};

struct basic {
	struct line *lines;
	int nlines, cap;
	int cur;		/* executing line index; -1 = immediate line */
	const char *p;
	char *imm;		/* the immediate-mode line being run */
	struct var *vars[NHASH];
	struct frame stack[STACK_MAX];
	int sp;
	int data_line;
	const char *data_p;
	int running;
	int stmts;
	int pending_key;
	uint64_t rng;
	uint64_t time_base;
	struct arch_jmpbuf err_jb;
	char err[160];
	int err_set;
	struct alloc allocs;	/* everything BASIC allocated, freed on exit */
	char path[VFS_PATH_MAX];
	int column;		/* output column, for TAB and commas */
	int at_start;		/* b->p sits at the start of a statement (after a jump or THEN) */
	int argc;
	char **argv;
};

int isxdigit_b(int c);

/* ---- allocation: tracked, so an error mid-expression can't leak ---------- */

static void *b_alloc(struct basic *b, size_t n)
{
	struct alloc *a = kmalloc(sizeof *a + n);
	if (!a)
		return NULL;
	a->next = b->allocs.next;
	a->prev = &b->allocs;
	b->allocs.next->prev = a;
	b->allocs.next = a;
	return a + 1;
}

static void b_free(struct basic *b, void *p)
{
	(void)b;
	if (!p)
		return;
	struct alloc *a = (struct alloc *)p - 1;
	a->prev->next = a->next;
	a->next->prev = a->prev;
	kfree(a);
}

static void b_free_all(struct basic *b)
{
	while (b->allocs.next != &b->allocs)
		b_free(b, b->allocs.next + 1);
}

static char *b_strdup(struct basic *b, const char *s, size_t n)
{
	char *d = b_alloc(b, n + 1);
	if (!d)
		return NULL;
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

/* ---- errors -------------------------------------------------------------- */

static int line_no(struct basic *b, int idx)
{
	if (idx < 0 || idx >= b->nlines)
		return 0;
	return b->lines[idx].num ? b->lines[idx].num : b->lines[idx].fileline;
}

static _Noreturn void berror(struct basic *b, const char *msg)
{
	if (!*msg)
		b->err[0] = 0;	/* quiet unwind (END inside a PROC) */
	else if (b->cur >= 0)
		snprintf(b->err, sizeof b->err, "%s at line %d", msg, line_no(b, b->cur));
	else
		snprintf(b->err, sizeof b->err, "%s", msg);
	b->err_set = 1;
	arch_longjmp(&b->err_jb, 1);
}

/* ---- values --------------------------------------------------------------- */

static void vfree(struct basic *b, struct val *v)
{
	if (v->t == T_STR)
		b_free(b, v->s);
	v->s = NULL;
}

static struct val vnum(double n)
{
	struct val v = { T_NUM, n, NULL };
	return v;
}

static struct val vstr(struct basic *b, const char *s, size_t n)
{
	struct val v = { T_STR, 0, b_strdup(b, s, n) };
	return v;
}

static const char *sval(struct val *v)
{
	return v->s ? v->s : "";
}

static double need_num(struct basic *b, struct val *v)
{
	if (v->t != T_NUM)
		berror(b, "Type mismatch");
	return v->n;
}

/* ---- lexing helpers -------------------------------------------------------- */

static void skip(struct basic *b)
{
	while (*b->p == ' ' || *b->p == '\t')
		b->p++;
}

static int at_end(struct basic *b)
{
	skip(b);
	return !*b->p || *b->p == ':';
}

static int is_idch(int c)
{
	return isalnum(c) || c == '_';
}

/* Does the text at p start with keyword kw (as a whole word)? */
static int kw_at(const char *p, const char *kw)
{
	size_t n = strlen(kw);
	if (strncasecmp(p, kw, n))
		return 0;
	/* "FN"/"PROC" glue to their names; '$' and '(' end keyword-functions */
	if (!strcasecmp(kw, "FN") || !strcasecmp(kw, "PROC"))
		return 1;
	if (kw[n - 1] == '$' || kw[n - 1] == '(')
		return 1;
	return !is_idch((unsigned char)p[n]) && p[n] != '$' && p[n] != '%';
}

static int accept_kw(struct basic *b, const char *kw)
{
	skip(b);
	if (kw_at(b->p, kw)) {
		b->p += strlen(kw);
		return 1;
	}
	return 0;
}

static int accept(struct basic *b, char c)
{
	skip(b);
	if (*b->p == c) {
		b->p++;
		return 1;
	}
	return 0;
}

static void expect(struct basic *b, char c)
{
	if (!accept(b, c)) {
		char m[32];
		snprintf(m, sizeof m, "Missing %c", c);
		berror(b, m);
	}
}

/* Words that end an expression or can't be variable names. */
static const char *const reserved[] = {
	"AND", "OR", "EOR", "DIV", "MOD", "NOT", "THEN", "ELSE", "TO", "STEP",
	"OF", "UNTIL", "ENDIF", "ENDWHILE", "ENDPROC", "PRINT", "IF", "FOR",
	"NEXT", "GOTO", "GOSUB", "RETURN", "LET", "DIM", "INPUT", "REM", "END",
	"STOP", "REPEAT", "WHILE", "DEF", "LOCAL", "DATA", "READ", "RESTORE",
	"CLS", "SYS", "OSCLI", "ON", "TRUE", "FALSE", NULL,
};

static int is_reserved(const char *p)
{
	for (int i = 0; reserved[i]; i++)
		if (kw_at(p, reserved[i]))
			return 1;
	return 0;
}

/* Read a variable name with its % or $ suffix. Returns length or 0. */
static int read_name(struct basic *b, char *out)
{
	skip(b);
	if (!isalpha((unsigned char)*b->p) && *b->p != '_')
		return 0;
	int n = 0;
	while (is_idch((unsigned char)*b->p) && n < NAME_MAX_B - 3)
		out[n++] = (char)toupper(*b->p++);
	if (*b->p == '%' || *b->p == '$')
		out[n++] = *b->p++;
	out[n] = 0;
	return n;
}

/* ---- variables -------------------------------------------------------------- */

static unsigned hash(const char *s)
{
	unsigned h = 5381;
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h % NHASH;
}

static struct var *find_var(struct basic *b, const char *name)
{
	for (struct var *v = b->vars[hash(name)]; v; v = v->next)
		if (!strcmp(v->name, name))
			return v;
	return NULL;
}

static struct var *make_var(struct basic *b, const char *name)
{
	struct var *v = find_var(b, name);
	if (v)
		return v;
	v = b_alloc(b, sizeof *v);
	if (!v)
		berror(b, "No room");
	memset(v, 0, sizeof *v);
	strncpy0(v->name, name, sizeof v->name);
	size_t l = strlen(name);
	int arr = l && name[l - 1] == '(';
	char last = arr ? (l > 1 ? name[l - 2] : 0) : name[l - 1];
	v->is_str = last == '$';
	v->is_int = last == '%';
	v->is_array = arr;
	unsigned h = hash(name);
	v->next = b->vars[h];
	b->vars[h] = v;
	return v;
}

static void assign(struct basic *b, struct var *v, int idx, struct val *val)
{
	if (v->is_str) {
		if (val->t != T_STR)
			berror(b, "Type mismatch");
		char *ns = b_strdup(b, sval(val), strlen(sval(val)));
		if (v->is_array) {
			b_free(b, v->sa[idx]);
			v->sa[idx] = ns;
		} else {
			b_free(b, v->s);
			v->s = ns;
		}
	} else {
		double n = need_num(b, val);
		if (v->is_int)
			n = f_trunc(n);
		if (v->is_array)
			v->na[idx] = n;
		else
			v->n = n;
	}
}

static struct val evaluate(struct basic *b);
static double eval_num(struct basic *b);
static void eval_str(struct basic *b, char *out, size_t n);

/* Parse "(i, j)" after an array name and return the flat index. */
static int array_index(struct basic *b, struct var *v)
{
	if (!v->ndims)
		berror(b, "Array not DIMmed");
	int idx = 0;
	for (int d = 0; d < v->ndims; d++) {
		if (d)
			expect(b, ',');
		double i = eval_num(b);
		if (i < 0 || i > v->dims[d])
			berror(b, "Subscript out of range");
		idx = idx * (v->dims[d] + 1) + (int)i;
	}
	expect(b, ')');
	return idx;
}

/* A variable reference on the left of '=' (or in INPUT/READ/FOR). */
static struct var *lvalue(struct basic *b, int *idx)
{
	char name[NAME_MAX_B];
	const char *save = b->p;
	if (!read_name(b, name) || is_reserved(b->p - strlen(name))) {
		b->p = save;
		return NULL;
	}
	*idx = 0;
	if (*b->p == '(') {
		b->p++;
		strcat0(name, "(", sizeof name);
		struct var *v = find_var(b, name);
		if (!v)
			berror(b, "Array not DIMmed");
		*idx = array_index(b, v);
		return v;
	}
	return make_var(b, name);
}

/* ---- program storage ------------------------------------------------------------ */

static void clear_program(struct basic *b)
{
	for (int i = 0; i < b->nlines; i++)
		b_free(b, b->lines[i].text);
	b->nlines = 0;
}

static void clear_vars(struct basic *b)
{
	for (int h = 0; h < NHASH; h++) {
		struct var *v = b->vars[h];
		while (v) {
			struct var *nx = v->next;
			b_free(b, v->s);
			if (v->sa)
				for (int i = 0; i < v->count; i++)
					b_free(b, v->sa[i]);
			b_free(b, v->sa);
			b_free(b, v->na);
			b_free(b, v);
			v = nx;
		}
		b->vars[h] = NULL;
	}
}

static int add_line(struct basic *b, int num, int fileline, const char *text)
{
	if (b->nlines == b->cap) {
		int nc = b->cap ? b->cap * 2 : 64;
		struct line *nl = b_alloc(b, sizeof(struct line) * (size_t)nc);
		if (!nl)
			return -1;
		if (b->lines) {
			memcpy(nl, b->lines, sizeof(struct line) * (size_t)b->nlines);
			b_free(b, b->lines);
		}
		b->lines = nl;
		b->cap = nc;
	}
	int at = b->nlines;
	if (num) {	/* keep numbered lines in order; replace duplicates */
		for (at = 0; at < b->nlines; at++) {
			if (b->lines[at].num == num) {
				b_free(b, b->lines[at].text);
				if (!*text) {	/* "10" alone deletes line 10 */
					memmove(&b->lines[at], &b->lines[at + 1],
					    sizeof(struct line) * (size_t)(b->nlines - at - 1));
					b->nlines--;
					return 0;
				}
				b->lines[at].text = b_strdup(b, text, strlen(text));
				return 0;
			}
			if (b->lines[at].num > num)
				break;
		}
		if (!*text)
			return 0;
		memmove(&b->lines[at + 1], &b->lines[at],
		    sizeof(struct line) * (size_t)(b->nlines - at));
	}
	b->lines[at].num = num;
	b->lines[at].fileline = fileline;
	b->lines[at].text = b_strdup(b, text, strlen(text));
	b->nlines++;
	return 0;
}

static int load_program(struct basic *b, const char *path)
{
	int err;
	size_t n;
	char *text = vfs_slurp(path, &n, &err);
	if (!text)
		return err;
	clear_program(b);
	int fileline = 0;
	char *p = text;
	while (*p) {
		char *e = p;
		while (*e && *e != '\n')
			e++;
		char save = *e;
		*e = 0;
		size_t l = strlen(p);
		while (l && (p[l - 1] == '\r' || p[l - 1] == ' '))
			p[--l] = 0;
		fileline++;
		const char *s = p;
		while (*s == ' ' || *s == '\t')
			s++;
		int num = 0;
		if (isdigit((unsigned char)*s)) {
			num = (int)strtol(s, &s, 10);
			if (*s == ' ')
				s++;
		}
		if (*s || num)
			add_line(b, num, fileline, s);
		*e = save;
		p = save ? e + 1 : e;
	}
	kfree(text);
	strncpy0(b->path, path, sizeof b->path);
	return 0;
}

static int find_line_num(struct basic *b, int num)
{
	for (int i = 0; i < b->nlines; i++)
		if (b->lines[i].num == num)
			return i;
	return -1;
}

static void jump_to_line(struct basic *b, int num)
{
	int i = find_line_num(b, num);
	if (i < 0)
		berror(b, "No such line");
	b->cur = i;
	b->p = b->lines[i].text;
	b->at_start = 1;
}

/* ---- output with column tracking ------------------------------------------------- */

static void out_str(struct basic *b, const char *s)
{
	for (; *s; s++) {
		con_putc(*s);
		if (*s == '\n' || *s == '\r')
			b->column = 0;
		else if (*s == '\b') {
			if (b->column)
				b->column--;
		} else
			b->column++;
	}
}

static void out_num(struct basic *b, double n)
{
	char buf[40];
	f_format(n, buf, sizeof buf, 9);
	out_str(b, buf);
}

/* ---- keyboard ---------------------------------------------------------------- */

static int get_key(struct basic *b, int wait_ms)
{
	if (b->pending_key >= 0) {
		int k = b->pending_key;
		b->pending_key = -1;
		return k;
	}
	uint64_t until = uptime_ms() + (uint64_t)(wait_ms < 0 ? 0 : wait_ms);
	for (;;) {
		int k = con_pollkey();
		if (k >= 0)
			return k;
		if (wait_ms >= 0 && uptime_ms() >= until)
			return -1;
		task_idle();
	}
}

/* Every so often: notice Escape and give other tasks a turn. */
static void housekeeping(struct basic *b)
{
	if (++b->stmts & 255)
		return;
	if (b->pending_key < 0) {
		int k = con_pollkey();
		if (k == 27 || k == 3)
			berror(b, "Escape");
		if (k >= 0)
			b->pending_key = k;
	}
	task_yield();
}

/* ---- expressions --------------------------------------------------------------- */

static struct val call_fn(struct basic *b, const char *name);

static double rnd(struct basic *b)
{
	b->rng ^= b->rng << 13;
	b->rng ^= b->rng >> 7;
	b->rng ^= b->rng << 17;
	return (double)(b->rng >> 11) / 9007199254740992.0;
}

static struct val str_func(struct basic *b, const char *name);

static struct val primary(struct basic *b)
{
	skip(b);
	const char *p = b->p;
	char c = *p;

	if (c == '(') {
		b->p++;
		struct val v = evaluate(b);
		expect(b, ')');
		return v;
	}
	if (c == '-') {
		b->p++;
		struct val v = primary(b);
		return vnum(-need_num(b, &v));
	}
	if (c == '+') {
		b->p++;
		return primary(b);
	}
	if (c == '"') {
		const char *s = ++b->p;
		char tmp[MAX_LINE];
		size_t n = 0;
		for (;;) {
			if (!*b->p)
				berror(b, "Missing \"");
			if (*b->p == '"') {
				if (b->p[1] == '"') {	/* "" inside a string is a quote */
					if (n < sizeof tmp - 1)
						tmp[n++] = '"';
					b->p += 2;
					continue;
				}
				break;
			}
			if (n < sizeof tmp - 1)
				tmp[n++] = *b->p;
			b->p++;
		}
		b->p++;
		(void)s;
		return vstr(b, tmp, n);
	}
	if (isdigit((unsigned char)c) || c == '.') {
		const char *e;
		double v = f_parse(p, &e);
		b->p = e;
		return vnum(v);
	}
	if (c == '&') {		/* hex */
		b->p++;
		long long v = 0;
		while (isxdigit_b(*b->p)) {
			char h = (char)toupper(*b->p++);
			v = v * 16 + (h <= '9' ? h - '0' : h - 'A' + 10);
		}
		return vnum((double)v);
	}
	if (c == '%' && (b->p[1] == '0' || b->p[1] == '1')) {	/* binary */
		b->p++;
		long long v = 0;
		while (*b->p == '0' || *b->p == '1')
			v = v * 2 + (*b->p++ - '0');
		return vnum((double)v);
	}

	/* keywords that are values or functions */
	struct { const char *kw; int id; } fns[] = {
		{ "ABS", 1 }, { "INT", 2 }, { "SGN", 3 }, { "SQR", 4 }, { "SIN", 5 },
		{ "COS", 6 }, { "TAN", 7 }, { "ATN", 8 }, { "EXP", 9 }, { "LN", 10 },
		{ "LOG", 11 }, { "RAD", 12 }, { "DEG", 13 }, { "ASN", 14 }, { "ACS", 15 },
		{ "LEN", 20 }, { "ASC", 21 }, { "VAL", 22 }, { "NOT", 30 }, { "RND", 40 },
		{ "PI", 41 }, { "TRUE", 42 }, { "FALSE", 43 }, { "TIME", 44 }, { "GET", 45 },
		{ "INKEY", 46 }, { "INSTR(", 47 }, { "POS", 48 }, { "VPOS", 49 },
		{ "ERL", 50 }, { "ARGC", 51 },
		{ NULL, 0 },
	};
	if (!kw_at(p, "GET$") && !kw_at(p, "INKEY$"))
	for (int i = 0; fns[i].kw; i++) {
		if (!kw_at(p, fns[i].kw))
			continue;
		b->p += strlen(fns[i].kw);
		int id = fns[i].id;
		if (id >= 1 && id <= 15) {
			struct val a = primary(b);
			double x = need_num(b, &a);
			switch (id) {
			case 1: return vnum(f_fabs(x));
			case 2: return vnum(f_floor(x));
			case 3: return vnum(x > 0 ? 1 : (x < 0 ? -1 : 0));
			case 4:
				if (x < 0)
					berror(b, "Negative root");
				return vnum(f_sqrt(x));
			case 5: return vnum(f_sin(x));
			case 6: return vnum(f_cos(x));
			case 7: return vnum(f_tan(x));
			case 8: return vnum(f_atan(x));
			case 9: return vnum(f_exp(x));
			case 10:
				if (x <= 0)
					berror(b, "Log range");
				return vnum(f_log(x));
			case 11:
				if (x <= 0)
					berror(b, "Log range");
				return vnum(f_log10(x));
			case 12: return vnum(x * 3.14159265358979323846 / 180);
			case 13: return vnum(x * 180 / 3.14159265358979323846);
			case 14: return vnum(f_asin(x));
			case 15: return vnum(f_acos(x));
			}
		}
		switch (id) {
		case 20: case 21: case 22: {
			struct val a = primary(b);
			if (a.t != T_STR)
				berror(b, "Type mismatch");
			const char *s = sval(&a);
			double r = id == 20 ? (double)strlen(s) :
			    id == 21 ? (*s ? (double)(unsigned char)*s : -1) : f_parse(s, NULL);
			vfree(b, &a);
			return vnum(r);
		}
		case 30: {
			struct val a = primary(b);
			return vnum((double)~(long long)need_num(b, &a));
		}
		case 40: {	/* RND, RND(n), RND(1) */
			if (*b->p == '(') {
				b->p++;
				double n = eval_num(b);
				expect(b, ')');
				if (n == 1)
					return vnum(rnd(b));
				if (n < 1)
					return vnum(0);
				return vnum(f_floor(rnd(b) * n) + 1);
			}
			return vnum((double)(int32_t)(b->rng = b->rng * 6364136223846793005ULL + 1));
		}
		case 41: return vnum(3.14159265358979323846);
		case 42: return vnum(-1);
		case 43: return vnum(0);
		case 44: return vnum((double)((uptime_ms() - b->time_base) / 10));	/* centiseconds */
		case 45: return vnum((double)get_key(b, -1));
		case 46: {
			double n = primary(b).n;
			return vnum((double)get_key(b, (int)(n * 10)));
		}
		case 47: {	/* INSTR(a$, b$ [, start]) */
			char hay[MAX_LINE], nee[MAX_LINE];
			eval_str(b, hay, sizeof hay);
			expect(b, ',');
			eval_str(b, nee, sizeof nee);
			int start = 1;
			if (accept(b, ','))
				start = (int)eval_num(b);
			expect(b, ')');
			if (start < 1)
				start = 1;
			size_t hl = strlen(hay), nl = strlen(nee);
			for (size_t i = (size_t)start - 1; i + nl <= hl; i++)
				if (!strncmp(hay + i, nee, nl))
					return vnum((double)(i + 1));
			return vnum(0);
		}
		case 48: return vnum((double)b->column);
		case 49: return vnum(0);
		case 50: return vnum((double)line_no(b, b->cur));
		case 51: return vnum((double)b->argc);
		}
	}

	if (kw_at(p, "FN")) {
		b->p += 2;
		char name[NAME_MAX_B];
		if (!read_name(b, name))
			berror(b, "Bad FN name");
		return call_fn(b, name);
	}

	/* string functions end in $ */
	struct val sv = str_func(b, p);
	if (sv.t == T_STR)
		return sv;

	/* variables */
	char name[NAME_MAX_B];
	if (is_reserved(p))
		berror(b, "Syntax error");
	if (!read_name(b, name))
		berror(b, "Syntax error");
	if (*b->p == '(') {
		b->p++;
		strcat0(name, "(", sizeof name);
		struct var *v = find_var(b, name);
		if (!v)
			berror(b, "Array not DIMmed");
		int idx = array_index(b, v);
		if (v->is_str)
			return vstr(b, v->sa[idx] ? v->sa[idx] : "", v->sa[idx] ? strlen(v->sa[idx]) : 0);
		return vnum(v->na[idx]);
	}
	struct var *v = find_var(b, name);
	if (!v)
		berror(b, "No such variable");
	if (v->is_str)
		return vstr(b, v->s ? v->s : "", v->s ? strlen(v->s) : 0);
	return vnum(v->n);
}

int isxdigit_b(int c)
{
	return isdigit(c) || (toupper(c) >= 'A' && toupper(c) <= 'F');
}

static struct val str_func(struct basic *b, const char *p)
{
	struct val none = { T_NUM, 0, NULL };
	char buf[MAX_LINE];
	if (kw_at(p, "CHR$")) {
		b->p += 4;
		struct val a = primary(b);
		buf[0] = (char)(int)need_num(b, &a);
		return vstr(b, buf, 1);
	}
	if (kw_at(p, "STR$")) {
		b->p += 4;
		int hex = accept(b, '~');
		struct val a = primary(b);
		double n = need_num(b, &a);
		if (hex)
			snprintf(buf, sizeof buf, "%llX", (long long)n);
		else
			f_format(n, buf, sizeof buf, 9);
		return vstr(b, buf, strlen(buf));
	}
	if (kw_at(p, "GET$")) {
		b->p += 4;
		buf[0] = (char)get_key(b, -1);
		return vstr(b, buf, 1);
	}
	if (kw_at(p, "INKEY$")) {
		b->p += 6;
		struct val a = primary(b);
		int k = get_key(b, (int)(need_num(b, &a) * 10));
		if (k < 0 || k > 255)
			return vstr(b, "", 0);
		buf[0] = (char)k;
		return vstr(b, buf, 1);
	}
	if (kw_at(p, "LEFT$(") || kw_at(p, "RIGHT$(") || kw_at(p, "MID$(")) {
		int which = toupper(*p) == 'L' ? 0 : toupper(*p) == 'R' ? 1 : 2;
		b->p += which == 0 ? 6 : which == 1 ? 7 : 5;
		char s[MAX_LINE];
		eval_str(b, s, sizeof s);
		int len = (int)strlen(s), n = 1, m = len;
		if (which == 2) {
			expect(b, ',');
			n = (int)eval_num(b);
			if (accept(b, ','))
				m = (int)eval_num(b);
		} else if (accept(b, ',')) {
			m = (int)eval_num(b);
		} else {
			m = len - 1;	/* LEFT$(a$) drops the last char, like BBC */
			if (which == 1)
				m = 1;
		}
		expect(b, ')');
		int start = which == 0 ? 0 : which == 1 ? len - m : n - 1;
		if (start < 0) start = 0;
		if (start > len) start = len;
		if (m < 0) m = 0;
		if (start + m > len) m = len - start;
		return vstr(b, s + start, (size_t)m);
	}
	if (kw_at(p, "STRING$(")) {
		b->p += 8;
		int n = (int)eval_num(b);
		expect(b, ',');
		char s[MAX_LINE];
		eval_str(b, s, sizeof s);
		expect(b, ')');
		size_t sl = strlen(s), o = 0;
		for (int i = 0; i < n && o + sl < sizeof buf; i++, o += sl)
			memcpy(buf + o, s, sl);
		return vstr(b, buf, o);
	}
	if (kw_at(p, "ARGV$(")) {
		b->p += 6;
		int i = (int)eval_num(b);
		expect(b, ')');
		const char *a = (i >= 0 && i < b->argc) ? b->argv[i] : "";
		return vstr(b, a, strlen(a));
	}
	if (kw_at(p, "UPPER$(") || kw_at(p, "LOWER$(")) {
		int up = toupper(*p) == 'U';
		b->p += 7;
		eval_str(b, buf, sizeof buf);
		expect(b, ')');
		for (char *c = buf; *c; c++)
			*c = (char)(up ? toupper(*c) : tolower(*c));
		return vstr(b, buf, strlen(buf));
	}
	return none;
}

static struct val power(struct basic *b)
{
	struct val l = primary(b);
	while (accept(b, '^')) {
		struct val r = primary(b);
		l = vnum(f_pow(need_num(b, &l), need_num(b, &r)));
	}
	return l;
}

static struct val term(struct basic *b)
{
	struct val l = power(b);
	for (;;) {
		skip(b);
		int op;
		if (*b->p == '*')
			op = 1;
		else if (*b->p == '/')
			op = 2;
		else if (kw_at(b->p, "DIV"))
			op = 3;
		else if (kw_at(b->p, "MOD"))
			op = 4;
		else
			return l;
		b->p += op <= 2 ? 1 : 3;
		struct val r = power(b);
		double x = need_num(b, &l), y = need_num(b, &r);
		if (op != 1 && y == 0)
			berror(b, "Division by zero");
		if (op == 1) x *= y;
		else if (op == 2) x /= y;
		else if (op == 3) x = (double)((long long)x / (long long)y);
		else x = (double)((long long)x % (long long)y);
		l = vnum(x);
	}
}

static struct val sum(struct basic *b)
{
	struct val l = term(b);
	for (;;) {
		skip(b);
		char op = *b->p;
		if (op != '+' && op != '-')
			return l;
		b->p++;
		struct val r = term(b);
		if (op == '+' && l.t == T_STR && r.t == T_STR) {
			size_t a = strlen(sval(&l)), c = strlen(sval(&r));
			char *s = b_alloc(b, a + c + 1);
			memcpy(s, sval(&l), a);
			memcpy(s + a, sval(&r), c + 1);
			vfree(b, &l);
			vfree(b, &r);
			l.t = T_STR;
			l.s = s;
			continue;
		}
		double x = need_num(b, &l), y = need_num(b, &r);
		l = vnum(op == '+' ? x + y : x - y);
	}
}

static struct val compare(struct basic *b)
{
	struct val l = sum(b);
	for (;;) {
		skip(b);
		int op = 0;	/* 1 = 2 <> 3 < 4 > 5 <= 6 >= */
		if (b->p[0] == '<' && b->p[1] == '>') op = 2;
		else if (b->p[0] == '<' && b->p[1] == '=') op = 5;
		else if (b->p[0] == '>' && b->p[1] == '=') op = 6;
		else if (b->p[0] == '=') op = 1;
		else if (b->p[0] == '<') op = 3;
		else if (b->p[0] == '>') op = 4;
		if (!op)
			return l;
		b->p += (op == 2 || op >= 5) ? 2 : 1;
		struct val r = sum(b);
		int c;
		if (l.t == T_STR && r.t == T_STR) {
			c = strcmp(sval(&l), sval(&r));
			vfree(b, &l);
			vfree(b, &r);
		} else {
			double x = need_num(b, &l), y = need_num(b, &r);
			c = x < y ? -1 : (x > y ? 1 : 0);
		}
		int res = op == 1 ? c == 0 : op == 2 ? c != 0 : op == 3 ? c < 0 :
		    op == 4 ? c > 0 : op == 5 ? c <= 0 : c >= 0;
		l = vnum(res ? -1 : 0);	/* BBC TRUE is -1 */
	}
}

static struct val and_expr(struct basic *b)
{
	struct val l = compare(b);
	while (accept_kw(b, "AND")) {
		struct val r = compare(b);
		l = vnum((double)((long long)need_num(b, &l) & (long long)need_num(b, &r)));
	}
	return l;
}

static struct val evaluate(struct basic *b)
{
	struct val l = and_expr(b);
	for (;;) {
		int op = accept_kw(b, "OR") ? 1 : accept_kw(b, "EOR") ? 2 : 0;
		if (!op)
			return l;
		struct val r = and_expr(b);
		long long x = (long long)need_num(b, &l), y = (long long)need_num(b, &r);
		l = vnum((double)(op == 1 ? (x | y) : (x ^ y)));
	}
}

static double eval_num(struct basic *b)
{
	struct val v = evaluate(b);
	return need_num(b, &v);
}

static void eval_str(struct basic *b, char *out, size_t n)
{
	struct val v = evaluate(b);
	if (v.t != T_STR)
		berror(b, "Type mismatch");
	strncpy0(out, sval(&v), n);
	vfree(b, &v);
}

/* ---- statements ----------------------------------------------------------- */

static void run_until(struct basic *b, int depth);

static void push(struct basic *b, int kind)
{
	if (b->sp >= STACK_MAX)
		berror(b, "Too many nested structures");
	struct frame *f = &b->stack[b->sp++];
	memset(f, 0, sizeof *f);
	f->kind = kind;
	f->line = b->cur;
	f->pos = b->p;
}

/* Skip to the end of the current statement (respecting strings). */
static void skip_stmt(struct basic *b)
{
	int q = 0;
	while (*b->p && (q || *b->p != ':')) {
		if (*b->p == '"')
			q = !q;
		b->p++;
	}
}

static void skip_line(struct basic *b)
{
	while (*b->p)
		b->p++;
}

/* Advance to the next statement; false at the end of the program. */
static int next_stmt(struct basic *b)
{
	skip(b);
	if (*b->p == ':') {
		b->p++;
		return 1;
	}
	if (*b->p)
		return 1;
	if (b->cur < 0 || b->cur + 1 >= b->nlines)
		return 0;
	b->cur++;
	b->p = b->lines[b->cur].text;
	return 1;
}

/*
 * Scan forward statement by statement for the end of a block, honouring
 * nesting. `open` and `close` are keywords like WHILE / ENDWHILE. If `alt`
 * is given (ELSE), stop there too at depth 0. Leaves b->p after the match.
 */
static int scan_block(struct basic *b, const char *open, const char *close,
    const char *alt, int block_if)
{
	int depth = 0;
	for (;;) {
		skip_stmt(b);
		if (!next_stmt(b))
			return 0;
		skip(b);
		const char *s = b->p;
		if (block_if && kw_at(s, "IF")) {
			/* nested block IF: line ends in THEN */
			const char *e = s;
			int q = 0;
			while (*e && (q || *e != ':')) {
				if (*e == '"')
					q = !q;
				e++;
			}
			const char *t = e;
			while (t > s && t[-1] == ' ')
				t--;
			if (t - s >= 4 && !strncasecmp(t - 4, "THEN", 4) && !*e)
				depth++;
			continue;
		}
		if (!block_if && kw_at(s, open)) {
			depth++;
			continue;
		}
		if (kw_at(s, close)) {
			if (!depth--) {
				b->p += strlen(close);
				return 1;
			}
			continue;
		}
		if (alt && !depth && kw_at(s, alt)) {
			b->p += strlen(alt);
			return 2;
		}
	}
}

static void do_print(struct basic *b)
{
	int newline = 1;
	for (;;) {
		if (at_end(b))
			break;
		skip(b);
		if (kw_at(b->p, "ELSE"))
			break;
		newline = 1;
		if (accept(b, ';')) {
			newline = 0;
			continue;
		}
		if (accept(b, ',')) {
			do
				out_str(b, " ");
			while (b->column % 10);
			newline = 0;
			continue;
		}
		if (accept(b, '\'')) {
			out_str(b, "\n");
			continue;
		}
		if (kw_at(b->p, "TAB(")) {
			b->p += 4;
			int x = (int)eval_num(b);
			if (accept(b, ',')) {
				int y = (int)eval_num(b);
				con_gotoxy(x, y);
				b->column = x;
			} else {
				if (b->column > x)
					out_str(b, "\n");
				while (b->column < x)
					out_str(b, " ");
			}
			expect(b, ')');
			newline = 1;
			continue;
		}
		if (kw_at(b->p, "SPC")) {
			b->p += 3;
			int n = (int)primary(b).n;
			while (n-- > 0)
				out_str(b, " ");
			continue;
		}
		int hex = accept(b, '~');
		struct val v = evaluate(b);
		if (v.t == T_STR) {
			out_str(b, sval(&v));
			vfree(b, &v);
		} else if (hex) {
			char buf[24];
			snprintf(buf, sizeof buf, "%llX", (long long)v.n);
			out_str(b, buf);
		} else {
			out_num(b, v.n);
		}
	}
	if (newline)
		out_str(b, "\n");
}

static void do_input(struct basic *b)
{
	for (;;) {
		if (at_end(b))
			return;
		int prompt_q = 1;
		skip(b);
		if (*b->p == '"') {
			struct val v = primary(b);
			out_str(b, sval(&v));
			vfree(b, &v);
			prompt_q = accept(b, ',') || accept(b, ';') ? 1 : 0;
			if (prompt_q)
				prompt_q = 0;	/* "prompt", var: no extra "?" in bdos */
		}
		int idx;
		struct var *v = lvalue(b, &idx);
		if (!v)
			berror(b, "Syntax error");
		if (prompt_q)
			out_str(b, "?");
		char line[MAX_LINE];
		if (read_line(line, sizeof line) < 0)
			line[0] = 0;
		b->column = 0;
		struct val in;
		if (v->is_str)
			in = vstr(b, line, strlen(line));
		else
			in = vnum(f_parse(line, NULL));
		assign(b, v, idx, &in);
		vfree(b, &in);
		if (!accept(b, ','))
			return;
	}
}

static void do_dim(struct basic *b)
{
	do {
		char name[NAME_MAX_B];
		if (!read_name(b, name) || !accept(b, '('))
			berror(b, "Bad DIM");
		strcat0(name, "(", sizeof name);
		struct var *v = make_var(b, name);
		if (v->ndims)
			berror(b, "Array already DIMmed");
		int count = 1;
		do {
			if (v->ndims >= 4)
				berror(b, "Too many dimensions");
			int d = (int)eval_num(b);
			if (d < 0 || d > 1000000)
				berror(b, "Bad DIM");
			v->dims[v->ndims++] = d;
			count *= d + 1;
		} while (accept(b, ','));
		expect(b, ')');
		v->count = count;
		if (v->is_str) {
			v->sa = b_alloc(b, sizeof(char *) * (size_t)count);
			if (!v->sa)
				berror(b, "No room");
			memset(v->sa, 0, sizeof(char *) * (size_t)count);
		} else {
			v->na = b_alloc(b, sizeof(double) * (size_t)count);
			if (!v->na)
				berror(b, "No room");
			memset(v->na, 0, sizeof(double) * (size_t)count);
		}
	} while (accept(b, ','));
}

/* READ: walk DATA statements in program order */
static int next_data(struct basic *b, char *out, size_t n)
{
	for (;;) {
		if (b->data_p && *b->data_p) {
			const char *p = b->data_p;
			while (*p == ' ')
				p++;
			if (*p == ',')
				p++;
			while (*p == ' ')
				p++;
			if (*p && *p != ':') {
				size_t o = 0;
				if (*p == '"') {
					p++;
					while (*p && *p != '"' && o + 1 < n)
						out[o++] = *p++;
					if (*p == '"')
						p++;
				} else {
					while (*p && *p != ',' && *p != ':' && o + 1 < n)
						out[o++] = *p++;
					while (o && out[o - 1] == ' ')
						o--;
				}
				out[o] = 0;
				b->data_p = p;
				return 1;
			}
		}
		/* find the next DATA statement */
		b->data_p = NULL;
		for (; b->data_line < b->nlines; b->data_line++) {
			const char *s = b->lines[b->data_line].text;
			while (*s == ' ')
				s++;
			if (kw_at(s, "DATA")) {
				b->data_p = s + 4;
				b->data_line++;
				break;
			}
		}
		if (!b->data_p)
			return 0;
	}
}

static void do_read(struct basic *b)
{
	do {
		int idx;
		struct var *v = lvalue(b, &idx);
		if (!v)
			berror(b, "Syntax error");
		char item[MAX_LINE];
		if (!next_data(b, item, sizeof item))
			berror(b, "Out of DATA");
		struct val val = v->is_str ? vstr(b, item, strlen(item)) : vnum(f_parse(item, NULL));
		assign(b, v, idx, &val);
		vfree(b, &val);
	} while (accept(b, ','));
}

/* ---- procedures and functions ------------------------------------------------- */

/* Find "DEF PROCname" / "DEF FNname"; returns line index, p after the name. */
static int find_def(struct basic *b, const char *kind, const char *name, const char **after)
{
	size_t kl = strlen(kind), nl = strlen(name);
	for (int i = 0; i < b->nlines; i++) {
		const char *s = b->lines[i].text;
		while (*s == ' ')
			s++;
		if (!kw_at(s, "DEF"))
			continue;
		s += 3;
		while (*s == ' ')
			s++;
		if (strncasecmp(s, kind, kl))
			continue;
		s += kl;
		if (!strncasecmp(s, name, nl) && !is_idch((unsigned char)s[nl]) &&
		    s[nl] != '$' && s[nl] != '%') {
			*after = s + nl;
			return i;
		}
		/* names with suffixes: compare exactly including % or $ */
		char got[NAME_MAX_B];
		int n = 0;
		while ((is_idch((unsigned char)s[n]) || s[n] == '$' || s[n] == '%') && n < NAME_MAX_B - 1) {
			got[n] = (char)toupper(s[n]);
			n++;
		}
		got[n] = 0;
		if (!strcmp(got, name)) {
			*after = s + n;
			return i;
		}
	}
	return -1;
}

static void save_local(struct basic *b, struct frame *f, struct var *v)
{
	struct saved *ns = b_alloc(b, sizeof(struct saved) * (size_t)(f->nsaved + 1));
	if (!ns)
		berror(b, "No room");
	if (f->saved) {
		memcpy(ns, f->saved, sizeof(struct saved) * (size_t)f->nsaved);
		b_free(b, f->saved);
	}
	f->saved = ns;
	ns[f->nsaved].v = v;
	ns[f->nsaved].n = v->n;
	ns[f->nsaved].s = v->s;		/* keep ownership: restored later */
	v->s = NULL;
	v->n = 0;
	f->nsaved++;
}

static void restore_locals(struct basic *b, struct frame *f)
{
	for (int i = f->nsaved - 1; i >= 0; i--) {
		struct var *v = f->saved[i].v;
		b_free(b, v->s);
		v->s = f->saved[i].s;
		v->n = f->saved[i].n;
	}
	b_free(b, f->saved);
	f->saved = NULL;
	f->nsaved = 0;
}

/*
 * Call PROCname or FNname: evaluate arguments here, bind them as locals in
 * the definition, then run until ENDPROC / "=expr" pops the frame.
 */
static struct val invoke(struct basic *b, const char *kind, const char *name, int kind_id)
{
	/* arguments, evaluated in the caller's context */
	struct val args[8];
	int nargs = 0;
	if (accept(b, '(')) {
		if (!accept(b, ')')) {
			do {
				if (nargs >= 8)
					berror(b, "Too many arguments");
				args[nargs++] = evaluate(b);
			} while (accept(b, ','));
			expect(b, ')');
		}
	}
	const char *after;
	int line = find_def(b, kind, name, &after);
	if (line < 0)
		berror(b, kind_id == F_PROC ? "No such PROC" : "No such FN");

	int depth = b->sp;
	push(b, kind_id);
	struct frame *f = &b->stack[b->sp - 1];
	b->cur = line;
	b->p = after;

	/* parameters */
	int i = 0;
	if (accept(b, '(')) {
		do {
			int idx;
			struct var *v = lvalue(b, &idx);
			if (!v || v->is_array)
				berror(b, "Bad parameter");
			if (i >= nargs)
				berror(b, "Not enough arguments");
			save_local(b, f, v);
			assign(b, v, 0, &args[i]);
			vfree(b, &args[i]);
			i++;
		} while (accept(b, ','));
		expect(b, ')');
	}
	if (i != nargs)
		berror(b, "Too many arguments");

	if (kind_id == F_FN && accept(b, '=')) {
		/* single-line function: DEF FNsq(x) = x * x */
		struct val r = evaluate(b);
		f = &b->stack[b->sp - 1];
		restore_locals(b, f);
		b->sp--;
		b->cur = f->line;
		b->p = f->pos;
		return r;
	}
	run_until(b, depth);
	/* the frame has been popped by ENDPROC or "=" */
	struct frame *done = &b->stack[depth];
	struct val r = done->ret;
	b->cur = done->line;
	b->p = done->pos;
	if (!b->running)
		berror(b, "");	/* END inside a PROC: unwind quietly */
	return r;
}

static struct val call_fn(struct basic *b, const char *name)
{
	return invoke(b, "FN", name, F_FN);
}

/* Pop frames down to the innermost PROC/FN (for ENDPROC / "="). */
static struct frame *pop_to(struct basic *b, int kind)
{
	while (b->sp > 0) {
		struct frame *f = &b->stack[--b->sp];
		if (f->kind == kind) {
			restore_locals(b, f);
			return f;
		}
		if (f->kind == F_PROC || f->kind == F_FN)
			break;
	}
	berror(b, kind == F_PROC ? "Not in a PROC" : "Not in a FN");
}

/* ---- SYS ---------------------------------------------------------------- */

static void do_sys(struct basic *b)
{
	char name[64];
	struct val nv = evaluate(b);
	if (nv.t != T_STR)
		berror(b, "SYS needs a name string");
	strncpy0(name, sval(&nv), sizeof name);
	vfree(b, &nv);

	struct sysval args[SYS_MAX_ARGS], ret;
	int argc = 0;
	while (accept(b, ',')) {
		if (argc >= SYS_MAX_ARGS)
			berror(b, "Too many SYS arguments");
		struct val a = evaluate(b);
		memset(&args[argc], 0, sizeof args[argc]);
		if (a.t == T_STR) {
			args[argc].is_str = 1;
			strncpy0(args[argc].s, sval(&a), sizeof args[argc].s);
		} else {
			args[argc].n = (long long)a.n;
			args[argc].f = a.n;
		}
		vfree(b, &a);
		argc++;
	}
	int r = sys_call(name, argc, args, &ret);
	if (r == -1) {
		char m[96];
		snprintf(m, sizeof m, "Unknown SYS call \"%s\"", name);
		berror(b, m);
	}
	if (r < 0) {
		char m[96];
		snprintf(m, sizeof m, "SYS \"%s\" failed", name);
		berror(b, m);
	}
	if (accept_kw(b, "TO")) {
		int idx;
		struct var *v = lvalue(b, &idx);
		if (!v)
			berror(b, "Syntax error");
		struct val out = ret.is_str ? vstr(b, ret.s, strlen(ret.s)) : vnum((double)ret.n);
		assign(b, v, idx, &out);
		vfree(b, &out);
	}
	b->column = 0;
}

/* ---- the statement dispatcher --------------------------------------------- */

static void do_if(struct basic *b)
{
	double c = eval_num(b);
	int then = accept_kw(b, "THEN");
	skip(b);
	if (then && (!*b->p)) {
		/* block IF: IF cond THEN <newline> ... [ELSE ...] ENDIF */
		if (c != 0)
			return;
		scan_block(b, "IF", "ENDIF", "ELSE", 1);
		return;
	}
	if (c != 0) {
		/* THEN 100 means GOTO 100 */
		if (isdigit((unsigned char)*b->p))
			jump_to_line(b, (int)eval_num(b));
		else if (*b->p && *b->p != ':')
			b->at_start = 1;	/* carry on with the rest of the line */
		return;
	}
	/* false: find ELSE on this line */
	int q = 0;
	while (*b->p) {
		if (*b->p == '"')
			q = !q;
		if (!q && kw_at(b->p, "ELSE")) {
			b->p += 4;
			skip(b);
			if (isdigit((unsigned char)*b->p))
				jump_to_line(b, (int)eval_num(b));
			else
				b->at_start = 1;
			return;
		}
		b->p++;
	}
}

static int statement(struct basic *b)
{
	skip(b);
	const char *s = b->p;
	if (!*s || *s == ':')
		return 1;
	housekeeping(b);

	if (*s == '*') {	/* *command goes to the shell */
		b->p++;
		shell_exec(b->p);
		skip_line(b);
		b->column = 0;
		return 1;
	}
	if (*s == '?') {
		b->p++;
		do_print(b);
		return 1;
	}
	if (*s == '=') {	/* =expr returns from a multi-line FN */
		b->p++;
		struct val r = evaluate(b);
		struct frame *f = pop_to(b, F_FN);
		f->ret = r;
		return 0;
	}

#define KW(w) accept_kw(b, w)
	if (KW("REM") || KW("DATA") || KW("DEF")) {
		skip_line(b);
		return 1;
	}
	if (KW("PRINT")) {
		do_print(b);
		return 1;
	}
	if (KW("INPUT")) {
		do_input(b);
		return 1;
	}
	if (KW("LET")) {
		/* fall through to assignment below */
	} else if (KW("IF")) {
		do_if(b);
		return 1;
	} else if (KW("ELSE")) {
		/* reached after a true single-line THEN, or a block's true part */
		const char *save = b->p;
		skip(b);
		if (!*b->p && b->cur >= 0) {	/* block ELSE on its own line */
			b->p = save;
			scan_block(b, "IF", "ENDIF", NULL, 1);
		} else {
			skip_line(b);
		}
		return 1;
	} else if (KW("ENDIF")) {
		return 1;
	} else if (KW("GOTO")) {
		jump_to_line(b, (int)eval_num(b));
		return 1;
	} else if (KW("GOSUB")) {
		int n = (int)eval_num(b);
		push(b, F_GOSUB);
		jump_to_line(b, n);
		return 1;
	} else if (KW("RETURN")) {
		while (b->sp > 0 && b->stack[b->sp - 1].kind != F_GOSUB) {
			int k = b->stack[b->sp - 1].kind;
			if (k == F_PROC || k == F_FN)
				break;
			b->sp--;
		}
		if (!b->sp || b->stack[b->sp - 1].kind != F_GOSUB)
			berror(b, "No GOSUB");
		struct frame *f = &b->stack[--b->sp];
		b->cur = f->line;
		b->p = f->pos;
		return 1;
	} else if (KW("ON")) {
		int n = (int)eval_num(b);
		int sub = KW("GOSUB");
		if (!sub && !KW("GOTO"))
			berror(b, "Syntax error");
		int target = 0;
		for (int i = 1;; i++) {
			int t = (int)eval_num(b);
			if (i == n)
				target = t;
			if (!accept(b, ','))
				break;
		}
		if (!target) {
			skip_stmt(b);
			return 1;
		}
		if (sub)
			push(b, F_GOSUB);
		jump_to_line(b, target);
		return 1;
	} else if (KW("FOR")) {
		int idx;
		struct var *v = lvalue(b, &idx);
		if (!v || v->is_str || v->is_array)
			berror(b, "Bad FOR variable");
		expect(b, '=');
		struct val start = vnum(eval_num(b));
		assign(b, v, 0, &start);
		if (!KW("TO"))
			berror(b, "Missing TO");
		double limit = eval_num(b);
		double step = KW("STEP") ? eval_num(b) : 1;
		double first = v->n;
		if ((step >= 0 && first > limit) || (step < 0 && first < limit)) {
			/* zero-trip loop: skip to the matching NEXT */
			if (!scan_block(b, "FOR", "NEXT", NULL, 0))
				berror(b, "Missing NEXT");
			skip_stmt(b);	/* the NEXT's variable list */
			return 1;
		}
		push(b, F_FOR);
		struct frame *f = &b->stack[b->sp - 1];
		f->v = v;
		f->limit = limit;
		f->step = step;
		f->line = b->cur;
		f->pos = b->p;
		return 1;
	} else if (KW("NEXT")) {
		int idx;
		struct var *want = lvalue(b, &idx);
		for (;;) {
			if (!b->sp || b->stack[b->sp - 1].kind != F_FOR)
				berror(b, "No FOR");
			struct frame *f = &b->stack[b->sp - 1];
			if (want && f->v != want) {
				b->sp--;	/* NEXT J closes an inner FOR I too */
				continue;
			}
			double n = f->v->n + f->step;
			if (f->v->is_int)
				n = f_trunc(n);
			f->v->n = n;
			if ((f->step >= 0 && n <= f->limit) || (f->step < 0 && n >= f->limit)) {
				b->cur = f->line;
				b->p = f->pos;
			} else {
				b->sp--;
				if (accept(b, ','))	/* NEXT I, J */
					continue;
			}
			return 1;
		}
	} else if (KW("REPEAT")) {
		push(b, F_REPEAT);
		return 1;
	} else if (KW("UNTIL")) {
		if (!b->sp || b->stack[b->sp - 1].kind != F_REPEAT)
			berror(b, "No REPEAT");
		double c = eval_num(b);
		struct frame *f = &b->stack[b->sp - 1];
		if (c == 0) {
			b->cur = f->line;
			b->p = f->pos;
		} else {
			b->sp--;
		}
		return 1;
	} else if (KW("WHILE")) {
		const char *cond = b->p;
		int line = b->cur;
		double c = eval_num(b);
		if (c == 0) {
			if (!scan_block(b, "WHILE", "ENDWHILE", NULL, 0))
				berror(b, "Missing ENDWHILE");
			return 1;
		}
		push(b, F_WHILE);
		struct frame *f = &b->stack[b->sp - 1];
		f->line = line;
		f->pos = cond;
		return 1;
	} else if (KW("ENDWHILE")) {
		if (!b->sp || b->stack[b->sp - 1].kind != F_WHILE)
			berror(b, "No WHILE");
		struct frame *f = &b->stack[--b->sp];
		const char *after = b->p;
		int after_line = b->cur;
		b->cur = f->line;
		b->p = f->pos;
		if (eval_num(b) != 0) {
			push(b, F_WHILE);
			struct frame *nf = &b->stack[b->sp - 1];
			nf->line = f->line;
			nf->pos = f->pos;
		} else {
			b->cur = after_line;
			b->p = after;
		}
		return 1;
	} else if (KW("DIM")) {
		do_dim(b);
		return 1;
	} else if (KW("PROC")) {
		char name[NAME_MAX_B];
		if (!read_name(b, name))
			berror(b, "Bad PROC name");
		invoke(b, "PROC", name, F_PROC);
		return 1;
	} else if (KW("ENDPROC")) {
		pop_to(b, F_PROC);
		return 0;
	} else if (KW("LOCAL")) {
		if (!b->sp)
			berror(b, "Not in a PROC");
		struct frame *f = NULL;
		for (int i = b->sp - 1; i >= 0 && !f; i--)
			if (b->stack[i].kind == F_PROC || b->stack[i].kind == F_FN)
				f = &b->stack[i];
		if (!f)
			berror(b, "Not in a PROC");
		do {
			int idx;
			struct var *v = lvalue(b, &idx);
			if (!v)
				berror(b, "Syntax error");
			save_local(b, f, v);
		} while (accept(b, ','));
		return 1;
	} else if (KW("READ")) {
		do_read(b);
		return 1;
	} else if (KW("RESTORE")) {
		b->data_line = 0;
		b->data_p = NULL;
		if (!at_end(b)) {
			int i = find_line_num(b, (int)eval_num(b));
			if (i >= 0)
				b->data_line = i;
		}
		return 1;
	} else if (KW("END") || KW("STOP")) {
		b->running = 0;
		return 0;
	} else if (KW("CLS")) {
		con_clear();
		b->column = 0;
		return 1;
	} else if (KW("COLOUR") || KW("COLOR")) {
		int fg = (int)eval_num(b), bg = 0;
		if (accept(b, ','))
			bg = (int)eval_num(b);
		con_attr(fg & 15, bg & 15);
		return 1;
	} else if (KW("WAIT")) {
		double cs = at_end(b) ? 0 : eval_num(b);
		sleep_ms((uint64_t)(cs * 10));
		return 1;
	} else if (KW("SYS")) {
		do_sys(b);
		return 1;
	} else if (KW("OSCLI")) {
		char cmd[MAX_LINE];
		eval_str(b, cmd, sizeof cmd);
		shell_exec(cmd);
		b->column = 0;
		return 1;
	} else if (KW("SWAP")) {
		int i1, i2;
		struct var *a = lvalue(b, &i1);
		expect(b, ',');
		struct var *c = lvalue(b, &i2);
		if (!a || !c || a->is_str != c->is_str || a->is_array || c->is_array)
			berror(b, "Bad SWAP");
		double tn = a->n;
		char *ts = a->s;
		a->n = c->n;
		a->s = c->s;
		c->n = tn;
		c->s = ts;
		return 1;
	} else if (KW("TIME")) {
		expect(b, '=');
		b->time_base = uptime_ms() - (uint64_t)(eval_num(b) * 10);
		return 1;
	} else if (KW("QUIT")) {
		b->running = 0;
		return 0;
	} else if (KW("TRACE") || KW("MODE") || KW("VDU") || KW("SOUND") || KW("ENVELOPE")) {
		skip_stmt(b);	/* BBC hardware we don't have */
		return 1;
	}
#undef KW

	/* assignment: [LET] var = expr, also var += expr, var -= expr */
	int idx;
	struct var *v = lvalue(b, &idx);
	if (!v)
		berror(b, "Mistake");
	skip(b);
	int op = 0;
	if (b->p[0] == '+' && b->p[1] == '=') op = '+';
	else if (b->p[0] == '-' && b->p[1] == '=') op = '-';
	if (op)
		b->p += 2;
	else
		expect(b, '=');
	struct val r = evaluate(b);
	if (op) {
		if (v->is_str && op == '+') {
			const char *old = v->is_array ? v->sa[idx] : v->s;
			char buf[MAX_LINE * 2];
			snprintf(buf, sizeof buf, "%s%s", old ? old : "", sval(&r));
			vfree(b, &r);
			r = vstr(b, buf, strlen(buf));
		} else {
			double cur = v->is_array ? v->na[idx] : v->n;
			r = vnum(op == '+' ? cur + need_num(b, &r) : cur - need_num(b, &r));
		}
	}
	assign(b, v, idx, &r);
	vfree(b, &r);
	return 1;
}

/* Run statements until the stack drops below `depth` or the program ends. */
static void run_until(struct basic *b, int depth)
{
	while (b->running) {
		if (!statement(b)) {
			if (b->sp <= depth || !b->running)
				return;
		}
		if (b->sp < depth)
			return;
		if (b->at_start) {
			b->at_start = 0;
			continue;
		}
		skip(b);
		if (*b->p && *b->p != ':') {
			/* a statement left junk behind */
			if (kw_at(b->p, "ELSE")) {
				skip_line(b);	/* true THEN part done: skip the ELSE part */
			} else {
				berror(b, "Syntax error");
			}
		}
		if (!next_stmt(b)) {
			b->running = 0;
			return;
		}
	}
}

static int run_program(struct basic *b, int start_line)
{
	b->sp = 0;
	b->data_line = 0;
	b->data_p = NULL;
	b->pending_key = -1;
	b->column = 0;
	b->running = 1;
	b->err_set = 0;
	b->at_start = 0;
	if (!b->nlines)
		return 0;
	b->cur = start_line;
	b->p = b->lines[start_line].text;
	if (arch_setjmp(&b->err_jb)) {
		b->running = 0;
		con_attr(7, 0);
		if (b->err[0]) {
			if (b->column)
				con_putc('\n');
			kprintf("%s\n", b->err);
		}
		return 1;
	}
	run_until(b, 0);
	con_attr(7, 0);
	return 0;
}

/* Run a single typed line in immediate mode. */
static void run_immediate(struct basic *b, char *line)
{
	b->sp = 0;
	b->running = 1;
	b->at_start = 0;
	b->pending_key = -1;
	b->cur = -1;
	b->p = line;
	if (arch_setjmp(&b->err_jb)) {
		b->running = 0;
		if (b->err[0])
			kprintf("%s\n", b->err);
		return;
	}
	while (b->running) {
		if (!statement(b) && b->cur < 0)
			break;
		if (b->at_start) {
			b->at_start = 0;
			continue;
		}
		skip(b);
		if (*b->p && *b->p != ':') {
			if (kw_at(b->p, "ELSE"))
				break;
			berror(b, "Syntax error");
		}
		if (!next_stmt(b))
			break;
	}
}

/* ---- sessions ------------------------------------------------------------- */

static struct basic *basic_new(void)
{
	struct basic *b = kzalloc(sizeof *b);
	if (!b)
		return NULL;
	b->allocs.next = b->allocs.prev = &b->allocs;
	b->rng = rtc_epoch() * 2654435761ULL + uptime_ms() + 1;
	b->time_base = uptime_ms();
	b->pending_key = -1;
	return b;
}

static void basic_free(struct basic *b)
{
	b_free_all(b);
	kfree(b);
}

int basic_run_file(const char *path, int argc, char **argv)
{
	struct basic *b = basic_new();
	if (!b)
		return 1;
	b->argc = argc;
	b->argv = argv;
	int r = load_program(b, path);
	if (r) {
		kprintf("%s - %s\n", vfs_strerror(r), path);
		basic_free(b);
		return 1;
	}
	r = run_program(b, 0);
	basic_free(b);
	return r;
}

static void list_program(struct basic *b, int from, int to)
{
	for (int i = 0; i < b->nlines; i++) {
		struct line *l = &b->lines[i];
		if (l->num && (l->num < from || l->num > to))
			continue;
		if (l->num)
			kprintf("%5d %s\n", l->num, l->text);
		else
			kprintf("      %s\n", l->text);
		if (con_pollkey() == 27)
			break;
	}
}

static void renumber(struct basic *b, int start, int step)
{
	int *old = kmalloc(sizeof(int) * (size_t)(b->nlines + 1));
	if (!old)
		return;
	for (int i = 0; i < b->nlines; i++) {
		old[i] = b->lines[i].num;
		b->lines[i].num = start + i * step;
	}
	/* rewrite GOTO/GOSUB/THEN/ELSE/RESTORE targets */
	static const char *const kws[] = { "GOTO", "GOSUB", "THEN", "ELSE", "RESTORE", NULL };
	for (int i = 0; i < b->nlines; i++) {
		char out[MAX_LINE * 2];
		size_t o = 0;
		const char *p = b->lines[i].text;
		int q = 0;
		while (*p && o + 16 < sizeof out) {
			if (*p == '"')
				q = !q;
			int hit = 0;
			for (int k = 0; !q && kws[k]; k++) {
				if (!kw_at(p, kws[k]))
					continue;
				size_t kl = strlen(kws[k]);
				memcpy(out + o, p, kl);
				o += kl;
				p += kl;
				for (;;) {
					while (*p == ' ')
						out[o++] = *p++;
					if (!isdigit((unsigned char)*p))
						break;
					int n = (int)strtol(p, &p, 10);
					int nn = n;
					for (int j = 0; j < b->nlines; j++)
						if (old[j] == n)
							nn = b->lines[j].num;
					o += (size_t)snprintf(out + o, sizeof out - o, "%d", nn);
					if (*p != ',')
						break;
					out[o++] = *p++;
				}
				hit = 1;
				break;
			}
			if (!hit)
				out[o++] = *p++;
		}
		out[o] = 0;
		b_free(b, b->lines[i].text);
		b->lines[i].text = b_strdup(b, out, o);
	}
	kfree(old);
}

static int save_program(struct basic *b, const char *path)
{
	size_t total = 0;
	for (int i = 0; i < b->nlines; i++)
		total += strlen(b->lines[i].text) + 12;
	char *buf = kmalloc(total + 1);
	if (!buf)
		return E_NOMEM;
	size_t o = 0;
	for (int i = 0; i < b->nlines; i++) {
		if (b->lines[i].num)
			o += (size_t)snprintf(buf + o, total + 1 - o, "%d %s\n", b->lines[i].num,
			    b->lines[i].text);
		else
			o += (size_t)snprintf(buf + o, total + 1 - o, "%s\n", b->lines[i].text);
	}
	int r = vfs_spew(path, buf, o);
	kfree(buf);
	return r;
}

/* LOAD "file" / SAVE "file": take the quoted or bare name after the command */
static int filename_arg(char *p, char *out, size_t n)
{
	while (*p == ' ')
		p++;
	if (*p == '"')
		p++;
	size_t o = 0;
	while (*p && *p != '"' && o + 1 < n)
		out[o++] = *p++;
	out[o] = 0;
	return o > 0;
}

int basic_immediate(void)
{
	struct basic *b = basic_new();
	if (!b)
		return 1;
	char line[MAX_LINE];
	kprintf("bdos BASIC - BBC style. RUN, LIST, NEW, LOAD \"f\", SAVE \"f\", QUIT.\n\n");
	for (;;) {
		kprintf(">");
		if (read_line(line, sizeof line) < 0)
			break;
		char *p = line;
		while (*p == ' ')
			p++;
		if (!*p)
			continue;
		if (isdigit((unsigned char)*p)) {
			int num = (int)strtol(p, (const char **)&p, 10);
			if (*p == ' ')
				p++;
			add_line(b, num, 0, p);
			continue;
		}
		char fname[VFS_PATH_MAX], abs[VFS_PATH_MAX];
		if (kw_at(p, "RUN")) {
			run_program(b, 0);
		} else if (kw_at(p, "LIST")) {
			int from = 0, to = 0x7FFFFFFF;
			const char *q = p + 4;
			while (*q == ' ')
				q++;
			if (isdigit((unsigned char)*q))
				from = to = (int)strtol(q, &q, 10);
			if (*q == ',' || *q == '-') {
				q++;
				to = isdigit((unsigned char)*q) ? (int)strtol(q, &q, 10) : 0x7FFFFFFF;
			}
			list_program(b, from, to);
		} else if (kw_at(p, "NEW")) {
			clear_program(b);
			clear_vars(b);
		} else if (kw_at(p, "LOAD") || kw_at(p, "CHAIN")) {
			int chain = toupper(*p) == 'C';
			if (!filename_arg(p + (chain ? 5 : 4), fname, sizeof fname) ||
			    resolve_path(fname, abs, sizeof abs)) {
				kprintf("Bad file name\n");
				continue;
			}
			int r = load_program(b, abs);
			if (r)
				kprintf("%s\n", vfs_strerror(r));
			else if (chain)
				run_program(b, 0);
		} else if (kw_at(p, "SAVE")) {
			if (!filename_arg(p + 4, fname, sizeof fname) ||
			    resolve_path(fname, abs, sizeof abs)) {
				kprintf("Bad file name\n");
				continue;
			}
			int r = save_program(b, abs);
			if (r)
				kprintf("%s\n", vfs_strerror(r));
		} else if (kw_at(p, "RENUMBER")) {
			renumber(b, 10, 10);
		} else if (kw_at(p, "QUIT") || kw_at(p, "EXIT")) {
			break;
		} else if (kw_at(p, "HELP")) {
			kprintf("Statements: PRINT INPUT LET IF THEN ELSE ENDIF FOR NEXT REPEAT UNTIL\n"
			    "  WHILE ENDWHILE GOTO GOSUB RETURN ON DIM DEF PROC FN LOCAL ENDPROC\n"
			    "  DATA READ RESTORE CLS COLOUR WAIT SYS OSCLI *cmd SWAP END\n"
			    "Functions: ABS INT SGN SQR SIN COS TAN ATN EXP LN LOG RAD DEG RND PI\n"
			    "  LEN ASC VAL CHR$ STR$ LEFT$ RIGHT$ MID$ INSTR STRING$ UPPER$ LOWER$\n"
			    "  GET GET$ INKEY INKEY$ TIME TRUE FALSE NOT AND OR EOR DIV MOD\n"
			    "Commands: RUN LIST NEW LOAD SAVE CHAIN RENUMBER QUIT\n");
		} else {
			run_immediate(b, p);
			con_attr(7, 0);
		}
	}
	basic_free(b);
	return 0;
}
