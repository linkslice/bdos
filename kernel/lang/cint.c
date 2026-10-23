/*
 * cint.c - run C programs straight from source: type HELLO.C and it runs.
 *
 * A tree-walking interpreter for a practical subset of C:
 *   types     char int long double void, pointers, arrays (any rank),
 *             struct, enum, typedef; unsigned is accepted
 *   code      functions and recursion, every statement except goto,
 *             the full operator set, casts, sizeof, initialisers
 *   cpp       #include (headers are built in), #define constants,
 *             #ifdef/#ifndef/#else/#endif
 *   library   stdio/string/stdlib/math/ctype basics, plus Turbo C's
 *             conio.h and dos.h (clrscr, gotoxy, getch, kbhit, delay)
 *   bdos      any function the program doesn't define is looked up in the
 *             SYS table: win_open() is SYS "Win_Open", desk_color() is
 *             SYS "Desk_Color" - the same calls BASIC programs use.
 *
 * Programs live in their own memory image, so a wild pointer is reported
 * as a runtime error instead of touching the kernel.
 */
#include "../kernel.h"
#include "../vfs.h"
#include "../sysapi.h"
#include "fmath.h"
#include "../gate.h"

static int cint_run_file_unlocked(const char *path, int argc, char **argv);

#define MEMSZ		(8u << 20)
#define STACKSZ		(1u << 20)
#define MAX_DEPTH	400
#define MAX_TOKENS	200000

/* ---- types ---------------------------------------------------------------------- */

enum { TY_VOID, TY_CHAR, TY_INT, TY_LONG, TY_DOUBLE, TY_PTR, TY_ARRAY, TY_FUNC, TY_STRUCT };

struct member {
	char name[32];
	struct type *ty;
	int offset;
	struct member *next;
};

struct type {
	int kind;
	int size, align;
	int is_unsigned;
	struct type *base;	/* pointer/array element, function return */
	int len;		/* array length (-1 unknown) */
	struct member *members;
	char tag[32];
	int complete;
};

/* ---- tokens ------------------------------------------------------------------------- */

enum { K_EOF, K_ID, K_INT, K_FLOAT, K_STR, K_CHAR, K_P };

struct tok {
	int kind;
	int line;
	long long ival;
	double fval;
	char *s;		/* identifier or string text */
	int slen;
	int p;			/* punctuator code */
};

/* punctuators, longest first */
static const char *const puncts[] = {
	"<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
	"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
	"+", "-", "*", "/", "%", "&", "|", "^", "~", "!", "<", ">", "=", "?", ":", ";", ",",
	".", "(", ")", "[", "]", "{", "}", "#",
};

/* ---- AST ------------------------------------------------------------------------------ */

enum {
	N_NUM, N_FNUM, N_STR, N_VAR, N_GVAR, N_FUNCREF, N_CALL, N_SYSCALL,
	N_BIN, N_LOG_AND, N_LOG_OR, N_ASSIGN, N_OPASSIGN, N_COND, N_COMMA,
	N_NEG, N_NOT, N_BITNOT, N_DEREF, N_ADDR, N_PREINC, N_PREDEC, N_POSTINC, N_POSTDEC,
	N_MEMBER, N_CAST, N_ARRAYINIT,
	S_EXPR, S_BLOCK, S_IF, S_WHILE, S_DO, S_FOR, S_RETURN, S_BREAK, S_CONTINUE,
	S_SWITCH, S_CASE, S_DEFAULT, S_DECL, S_EMPTY,
};

struct func;

struct node {
	int kind, line, op;
	struct type *ty;
	struct node *a, *b, *c, *d;
	struct node *next;
	long long ival;
	double fval;
	int offset;		/* local/global address offset, member offset */
	struct func *fn;
	char *name;
	struct node *init;	/* S_DECL initialiser */
};

struct func {
	char name[64];
	struct type *ret;
	struct type *ptypes[16];
	int poffs[16];
	int nparams;
	int varargs;
	struct node *body;
	int frame;
	int builtin;		/* index into builtins, or 0 */
	struct func *next;
	int defined;
};

/* ---- scopes ------------------------------------------------------------------------------ */

enum { SY_VAR, SY_GVAR, SY_TYPEDEF, SY_ENUM, SY_FUNC };

struct sym {
	char name[64];
	int kind;
	struct type *ty;
	int offset;
	long long val;
	struct func *fn;
	struct sym *next;
};

struct scope {
	struct sym *syms;
	struct type *tags[32];
	int ntags;
	struct scope *up;
};

/* ---- the interpreter ------------------------------------------------------------------- */

struct cint {
	char path[VFS_PATH_MAX];
	/* lexing */
	struct tok *toks;
	int ntoks, pos;
	/* macros */
	struct macro {
		char name[64];
		int start, count;	/* range in mtoks */
		int is_func, nparams;
		char params[8][32];
	} macros[256];
	int nmacros;
	struct tok *mtoks;
	int nmtoks;
	/* parsing */
	struct scope *scope;
	struct func *funcs;
	struct func *cur_fn;
	int frame_off;
	int max_frame;
	int loop_depth, switch_depth;
	/* memory */
	uint8_t *mem;
	uint32_t data_top;	/* globals and strings grow up from 16 */
	uint32_t heap_base, heap_top, heap_end;
	uint32_t sp, fp;
	int depth;
	/* runtime */
	long long ret_i;
	double ret_d;
	struct type *ret_ty;
	int line;
	int steps;
	int exiting, exit_code;
	struct arch_jmpbuf jb;
	char err[200];
	/* allocations made during parsing, freed at the end */
	struct pnode {
		struct pnode *next;
	} *pool;
	uint32_t rng;
	int col;
};

/* every allocation goes through here so one free releases the lot */
static void *calloc_c(struct cint *c, size_t n)
{
	struct pnode *p = kzalloc(sizeof *p + n);
	if (!p) {
		snprintf(c->err, sizeof c->err, "out of memory");
		arch_longjmp(&c->jb, 1);
	}
	p->next = c->pool;
	c->pool = p;
	return p + 1;
}

static _Noreturn void cerr(struct cint *c, int line, const char *fmt, ...)
{
	char msg[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	snprintf(c->err, sizeof c->err, "%s:%d: %s", vfs_basename(c->path), line, msg);
	arch_longjmp(&c->jb, 1);
}

/* ---- types ----------------------------------------------------------------------------- */

static struct type ty_void = { TY_VOID, 1, 1, 0, 0, 0, 0, "", 1 };
static struct type ty_char = { TY_CHAR, 1, 1, 0, 0, 0, 0, "", 1 };
static struct type ty_int = { TY_INT, 4, 4, 0, 0, 0, 0, "", 1 };
static struct type ty_long = { TY_LONG, 8, 8, 0, 0, 0, 0, "", 1 };
static struct type ty_double = { TY_DOUBLE, 8, 8, 0, 0, 0, 0, "", 1 };

static struct type *ptr_to(struct cint *c, struct type *base)
{
	struct type *t = calloc_c(c, sizeof *t);
	t->kind = TY_PTR;
	t->size = t->align = 8;
	t->base = base;
	t->complete = 1;
	return t;
}

static struct type *array_of(struct cint *c, struct type *base, int len)
{
	struct type *t = calloc_c(c, sizeof *t);
	t->kind = TY_ARRAY;
	t->base = base;
	t->len = len;
	t->size = len < 0 ? 0 : base->size * len;
	t->align = base->align;
	t->complete = len >= 0;
	return t;
}

static int is_int(struct type *t) { return t->kind == TY_CHAR || t->kind == TY_INT || t->kind == TY_LONG; }
static int is_arith(struct type *t) { return is_int(t) || t->kind == TY_DOUBLE; }
static int is_ptr(struct type *t) { return t->kind == TY_PTR || t->kind == TY_ARRAY; }

/* ---- lexer + preprocessor ---------------------------------------------------------------- */

static int isxdigit_c(int ch)
{
	return isdigit(ch) || (toupper(ch) >= 'A' && toupper(ch) <= 'F');
}

static int find_macro(struct cint *c, const char *name, int len)
{
	for (int i = 0; i < c->nmacros; i++)
		if ((int)strlen(c->macros[i].name) == len && !strncmp(c->macros[i].name, name, (size_t)len))
			return i;
	return -1;
}

static int lex_one(struct cint *c, const char **pp, int *line, struct tok *t)
{
	const char *p = *pp;
	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\f' || *p == '\v')
			p++;
		if (*p == '\\' && p[1] == '\n') {
			p += 2;
			(*line)++;
			continue;
		}
		if (p[0] == '/' && p[1] == '/') {
			while (*p && *p != '\n')
				p++;
			continue;
		}
		if (p[0] == '/' && p[1] == '*') {
			p += 2;
			while (*p && !(p[0] == '*' && p[1] == '/')) {
				if (*p == '\n')
					(*line)++;
				p++;
			}
			if (*p)
				p += 2;
			continue;
		}
		break;
	}
	memset(t, 0, sizeof *t);
	t->line = *line;
	if (!*p) {
		t->kind = K_EOF;
		*pp = p;
		return 0;
	}
	if (*p == '\n') {
		*pp = p;
		return 2;	/* newline: the caller tracks lines for # handling */
	}
	if (isalpha((unsigned char)*p) || *p == '_') {
		const char *s = p;
		while (isalnum((unsigned char)*p) || *p == '_')
			p++;
		t->kind = K_ID;
		t->slen = (int)(p - s);
		t->s = calloc_c(c, (size_t)t->slen + 1);
		memcpy(t->s, s, (size_t)t->slen);
		*pp = p;
		return 1;
	}
	if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
		const char *s = p;
		int is_float = 0;
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
			p += 2;
			long long v = 0;
			while (isxdigit_c(*p)) {
				int d = isdigit((unsigned char)*p) ? *p - '0' : (toupper(*p) - 'A' + 10);
				v = v * 16 + d;
				p++;
			}
			t->ival = v;
		} else {
			while (isdigit((unsigned char)*p))
				p++;
			if (*p == '.' || *p == 'e' || *p == 'E') {
				is_float = 1;
				t->fval = f_parse(s, &p);
			} else if (s[0] == '0' && p - s > 1) {
				long long v = 0;
				for (const char *q = s; q < p; q++)
					v = v * 8 + (*q - '0');
				t->ival = v;
			} else {
				long long v = 0;
				for (const char *q = s; q < p; q++)
					v = v * 10 + (*q - '0');
				t->ival = v;
			}
		}
		while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L' || *p == 'f' || *p == 'F') {
			if (*p == 'l' || *p == 'L')
				t->slen = 1;	/* a long constant */
			p++;
		}
		t->kind = is_float ? K_FLOAT : K_INT;
		*pp = p;
		return 1;
	}
	if (*p == '"' || *p == '\'') {
		char q = *p++;
		char buf[1024];
		int n = 0;
		while (*p && *p != q && *p != '\n') {
			int ch = (unsigned char)*p++;
			if (ch == '\\') {
				ch = (unsigned char)*p++;
				switch (ch) {
				case 'n': ch = '\n'; break;
				case 't': ch = '\t'; break;
				case 'r': ch = '\r'; break;
				case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
					int v = ch - '0';
					for (int k = 0; k < 2 && *p >= '0' && *p <= '7'; k++)
						v = v * 8 + (*p++ - '0');
					ch = v;
					break;
				}
				case 'x': {
					int v = 0;
					while (isxdigit_c(*p)) {
						v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : toupper(*p) - 'A' + 10);
						p++;
					}
					ch = v;
					break;
				}
				case 'a': ch = 7; break;
				case 'b': ch = 8; break;
				case 'f': ch = 12; break;
				case 'v': ch = 11; break;
				case 'e': ch = 27; break;
				default: break;	/* \\ \' \" \? */
				}
			}
			if (n < (int)sizeof buf - 1)
				buf[n++] = (char)ch;
		}
		if (*p == q)
			p++;
		else
			cerr(c, *line, "unterminated %s", q == '"' ? "string" : "character constant");
		if (q == '\'') {
			t->kind = K_CHAR;
			t->ival = n ? (signed char)buf[0] : 0;
		} else {
			t->kind = K_STR;
			t->s = calloc_c(c, (size_t)n + 1);
			memcpy(t->s, buf, (size_t)n);
			t->slen = n;
		}
		*pp = p;
		return 1;
	}
	for (size_t i = 0; i < NELEM(puncts); i++) {
		size_t l = strlen(puncts[i]);
		if (!strncmp(p, puncts[i], l)) {
			t->kind = K_P;
			t->p = (int)i;
			*pp = p + l;
			return 1;
		}
	}
	cerr(c, *line, "stray '%c' in program", *p);
}


static int P(const char *s)
{
	for (size_t i = 0; i < NELEM(puncts); i++)
		if (!strcmp(puncts[i], s))
			return (int)i;
	return -1;
}

static void push_tok(struct cint *c, struct tok *t)
{
	if (c->ntoks >= MAX_TOKENS)
		cerr(c, t->line, "program too large");
	c->toks[c->ntoks++] = *t;
}

/* Expand macros in a token list and append the result to the program.
 * Function-like macros take their arguments from the same list. */
static void expand_list(struct cint *c, const struct tok *in, int n, int line, int depth)
{
	if (depth > 16)
		cerr(c, line, "macros nest too deeply");
	for (int i = 0; i < n; i++) {
		struct tok t = in[i];
		t.line = line;
		if (t.kind == K_ID && !strcmp(t.s, "__LINE__")) {
			t.kind = K_INT;
			t.ival = line;
		}
		int mi = t.kind == K_ID ? find_macro(c, t.s, t.slen) : -1;
		if (mi < 0) {
			push_tok(c, &t);
			continue;
		}
		struct macro *m = &c->macros[mi];
		const struct tok *body = c->mtoks + m->start;
		if (!m->is_func) {
			expand_list(c, body, m->count, line, depth + 1);
			continue;
		}
		if (i + 1 >= n || in[i + 1].kind != K_P || in[i + 1].p != P("(")) {
			push_tok(c, &t);
			continue;
		}
		/* split the arguments at top-level commas */
		int astart[8], alen[8], na = 0, lvl = 0, j = i + 2;
		astart[0] = j;
		for (; j < n; j++) {
			const struct tok *a = &in[j];
			if (a->kind == K_P && a->p == P("(")) lvl++;
			else if (a->kind == K_P && a->p == P(")")) {
				if (!lvl)
					break;
				lvl--;
			} else if (a->kind == K_P && a->p == P(",") && !lvl) {
				if (na >= 7)
					cerr(c, line, "too many macro arguments");
				alen[na] = j - astart[na];
				astart[++na] = j + 1;
			}
		}
		if (j >= n)
			cerr(c, line, "unterminated call of macro %s", m->name);
		alen[na] = j - astart[na];
		if (j > i + 2 || m->nparams)
			na++;
		if (na != m->nparams)
			cerr(c, line, "macro %s takes %d argument%s", m->name, m->nparams,
			    m->nparams == 1 ? "" : "s");
		/* substitute, then rescan */
		struct tok *out = kmalloc(sizeof(struct tok) * 1024);
		int no = 0;
		for (int k = 0; k < m->count; k++) {
			const struct tok *b = &body[k];
			int pi = -1;
			if (b->kind == K_ID)
				for (int q = 0; q < m->nparams; q++)
					if (!strcmp(b->s, m->params[q]))
						pi = q;
			if (pi < 0) {
				if (no < 1024)
					out[no++] = *b;
				continue;
			}
			for (int q = 0; q < alen[pi] && no < 1024; q++)
				out[no++] = in[astart[pi] + q];
		}
		/* the macro's own name isn't expanded again inside itself */
		char saved = m->name[0];
		m->name[0] = 0;
		expand_list(c, out, no, line, depth + 1);
		m->name[0] = saved;
		kfree(out);
		i = j;
	}
}

static void lex(struct cint *c, const char *src)
{
	const char *p = src;
	int line = 1, at_bol = 1;
	int skip_depth = 0, depth = 0;	/* #ifdef nesting */
	struct tok t;
	for (;;) {
		int r = lex_one(c, &p, &line, &t);
		if (r == 0)
			break;
		if (r == 2) {
			p++;
			line++;
			at_bol = 1;
			continue;
		}
		if (at_bol && t.kind == K_P && t.p == P("#")) {
			/* a preprocessor line */
			struct tok d;
			int dl = line;
			if (lex_one(c, &p, &dl, &d) != 1 || d.kind != K_ID) {
				while (*p && *p != '\n')
					p++;
				continue;
			}
			if (!strcmp(d.s, "ifdef") || !strcmp(d.s, "ifndef")) {
				struct tok n;
				lex_one(c, &p, &dl, &n);
				int defd = n.kind == K_ID && find_macro(c, n.s, n.slen) >= 0;
				if (!strcmp(n.s ? n.s : "", "__BDOS__"))
					defd = 1;
				depth++;
				if (!skip_depth && (d.s[2] == 'n' ? defd : !defd))
					skip_depth = depth;
			} else if (!strcmp(d.s, "if")) {
				depth++;	/* only #if 0 / #if 1 */
				struct tok n;
				lex_one(c, &p, &dl, &n);
				if (!skip_depth && n.kind == K_INT && n.ival == 0)
					skip_depth = depth;
			} else if (!strcmp(d.s, "else")) {
				if (skip_depth == depth)
					skip_depth = 0;
				else if (!skip_depth)
					skip_depth = depth;
			} else if (!strcmp(d.s, "endif")) {
				if (skip_depth == depth)
					skip_depth = 0;
				if (depth)
					depth--;
			} else if (skip_depth) {
				/* ignore */
			} else if (!strcmp(d.s, "define")) {
				struct tok n;
				lex_one(c, &p, &dl, &n);
				if (n.kind != K_ID)
					cerr(c, line, "bad #define");
				if (c->nmacros >= (int)NELEM(c->macros))
					cerr(c, line, "too many #defines");
				struct macro *m = &c->macros[c->nmacros++];
				memset(m, 0, sizeof *m);
				strncpy0(m->name, n.s, sizeof m->name);
				if (*p == '(') {	/* function-like: NAME(a, b) */
					p++;
					m->is_func = 1;
					struct tok a;
					while (lex_one(c, &p, &dl, &a) == 1) {
						if (a.kind == K_P && a.p == P(")"))
							break;
						if (a.kind == K_P && a.p == P(","))
							continue;
						if (a.kind != K_ID || m->nparams >= 8)
							cerr(c, line, "bad macro parameter list");
						strncpy0(m->params[m->nparams++], a.s, 32);
					}
				}
				m->start = c->nmtoks;
				struct tok v;
				while (lex_one(c, &p, &dl, &v) == 1) {
					if (c->nmtoks >= 4096)
						cerr(c, line, "macros too long");
					c->mtoks[c->nmtoks++] = v;
					m->count++;
				}
			} else if (!strcmp(d.s, "undef")) {
				struct tok n;
				lex_one(c, &p, &dl, &n);
				int mi = n.kind == K_ID ? find_macro(c, n.s, n.slen) : -1;
				if (mi >= 0)
					c->macros[mi].name[0] = 0;
			}
			/* #include, #pragma, #error: the standard headers are built in */
			while (*p && *p != '\n')
				p++;
			continue;
		}
		at_bol = 0;
		if (skip_depth)
			continue;
		if (t.kind == K_ID && !strcmp(t.s, "__LINE__")) {
			t.kind = K_INT;
			t.ival = t.line;
		}
		if (t.kind == K_ID) {
			int mi = find_macro(c, t.s, t.slen);
			if (mi >= 0) {
				/* gather the invocation (with its arguments) and expand it */
				struct tok inv[512];
				int ni = 0;
				inv[ni++] = t;
				if (c->macros[mi].is_func) {
					const char *save = p;
					int sline = line;
					struct tok a;
					int r2;
					while ((r2 = lex_one(c, &p, &line, &a)) == 2)
						p++, line++;
					if (r2 != 1 || a.kind != K_P || a.p != P("(")) {
						p = save;	/* the name alone: not a call */
						line = sline;
						push_tok(c, &t);
						continue;
					}
					inv[ni++] = a;
					int depth = 1;
					while (depth) {
						r2 = lex_one(c, &p, &line, &a);
						if (r2 == 2) {
							p++;
							line++;
							continue;
						}
						if (r2 == 0)
							cerr(c, t.line, "unterminated call of macro %s", t.s);
						if (a.kind == K_P && a.p == P("("))
							depth++;
						if (a.kind == K_P && a.p == P(")"))
							depth--;
						if (ni >= (int)NELEM(inv))
							cerr(c, t.line, "macro call too long");
						inv[ni++] = a;
					}
				}
				expand_list(c, inv, ni, t.line, 0);
				continue;
			}
		}
		/* adjacent string literals join */
		if (t.kind == K_STR && c->ntoks && c->toks[c->ntoks - 1].kind == K_STR) {
			struct tok *prev = &c->toks[c->ntoks - 1];
			char *j = calloc_c(c, (size_t)(prev->slen + t.slen) + 1);
			memcpy(j, prev->s, (size_t)prev->slen);
			memcpy(j + prev->slen, t.s, (size_t)t.slen);
			prev->s = j;
			prev->slen += t.slen;
			continue;
		}
		push_tok(c, &t);
	}
	t.kind = K_EOF;
	t.line = line;
	push_tok(c, &t);
}

/* ---- parser helpers -------------------------------------------------------------------- */

static struct tok *peek(struct cint *c) { return &c->toks[c->pos]; }
static struct tok *peekn(struct cint *c, int n)
{
	int i = MIN(c->pos + n, c->ntoks - 1);
	return &c->toks[i];
}
static struct tok *next(struct cint *c) { return &c->toks[c->pos < c->ntoks - 1 ? c->pos++ : c->pos]; }

static int is_p(struct cint *c, const char *s)
{
	struct tok *t = peek(c);
	return t->kind == K_P && t->p == P(s);
}

static int accept(struct cint *c, const char *s)
{
	if (is_p(c, s)) {
		c->pos++;
		return 1;
	}
	return 0;
}

static void expect(struct cint *c, const char *s)
{
	if (!accept(c, s))
		cerr(c, peek(c)->line, "expected '%s'", s);
}

static int is_id(struct cint *c, const char *s)
{
	struct tok *t = peek(c);
	return t->kind == K_ID && !strcmp(t->s, s);
}

static int accept_id(struct cint *c, const char *s)
{
	if (is_id(c, s)) {
		c->pos++;
		return 1;
	}
	return 0;
}

static struct node *new_node(struct cint *c, int kind, int line)
{
	struct node *n = calloc_c(c, sizeof *n);
	n->kind = kind;
	n->line = line;
	return n;
}

/* ---- scopes ------------------------------------------------------------------------------- */

static void push_scope(struct cint *c)
{
	struct scope *s = calloc_c(c, sizeof *s);
	s->up = c->scope;
	c->scope = s;
}

static void pop_scope(struct cint *c)
{
	c->scope = c->scope->up;
}

static struct sym *lookup(struct cint *c, const char *name)
{
	for (struct scope *s = c->scope; s; s = s->up)
		for (struct sym *y = s->syms; y; y = y->next)
			if (!strcmp(y->name, name))
				return y;
	return NULL;
}

static struct sym *add_sym(struct cint *c, const char *name, int kind, struct type *ty)
{
	struct sym *y = calloc_c(c, sizeof *y);
	strncpy0(y->name, name, sizeof y->name);
	y->kind = kind;
	y->ty = ty;
	y->next = c->scope->syms;
	c->scope->syms = y;
	return y;
}

static struct type *find_tag(struct cint *c, const char *tag)
{
	for (struct scope *s = c->scope; s; s = s->up)
		for (int i = 0; i < s->ntags; i++)
			if (!strcmp(s->tags[i]->tag, tag))
				return s->tags[i];
	return NULL;
}

static struct func *find_func(struct cint *c, const char *name)
{
	for (struct func *f = c->funcs; f; f = f->next)
		if (!strcmp(f->name, name))
			return f;
	return NULL;
}

/* ---- memory ----------------------------------------------------------------------------------- */

static void check_addr(struct cint *c, uint64_t a, int n)
{
	if (a < 16 || a + (uint64_t)n > MEMSZ)
		cerr(c, c->line, a < 16 ? "null pointer used" : "pointer out of bounds");
}

static uint32_t alloc_data(struct cint *c, int size, int align)
{
	if (align < 1)
		align = 1;
	c->data_top = (c->data_top + (uint32_t)align - 1) & ~((uint32_t)align - 1);
	uint32_t a = c->data_top;
	c->data_top += (uint32_t)MAX(size, 1);
	if (c->data_top >= c->heap_base)
		cerr(c, c->line, "too much global data");
	return a;
}

static uint32_t intern_string(struct cint *c, const char *s, int len)
{
	uint32_t a = alloc_data(c, len + 1, 1);
	memcpy(c->mem + a, s, (size_t)len);
	c->mem[a + len] = 0;
	return a;
}

/* malloc inside the program's memory: first fit over a free list in the heap */
struct mhdr {
	uint32_t size;	/* payload bytes; top bit = free */
	uint32_t pad;
};

static uint32_t c_malloc(struct cint *c, uint64_t n)
{
	if (n > MEMSZ)
		return 0;
	uint32_t want = (uint32_t)((n + 15) & ~15ULL);
	uint32_t a = c->heap_base;
	while (a < c->heap_top) {
		struct mhdr *h = (struct mhdr *)(c->mem + a);
		uint32_t sz = h->size & 0x7FFFFFFF;
		if ((h->size & 0x80000000) && sz >= want) {
			if (sz >= want + 32) {	/* split */
				struct mhdr *rest = (struct mhdr *)(c->mem + a + 16 + want);
				rest->size = 0x80000000 | (sz - want - 16);
				sz = want;
			}
			h->size = sz;
			memset(c->mem + a + 16, 0, sz);
			return a + 16;
		}
		a += 16 + sz;
	}
	if (c->heap_top + 16 + want >= c->heap_end)
		return 0;
	struct mhdr *h = (struct mhdr *)(c->mem + c->heap_top);
	h->size = want;
	uint32_t r = c->heap_top + 16;
	c->heap_top += 16 + want;
	memset(c->mem + r, 0, want);
	return r;
}

static void c_free(struct cint *c, uint64_t p)
{
	if (!p)
		return;
	if (p < c->heap_base + 16 || p >= c->heap_top)
		cerr(c, c->line, "free() of a pointer malloc didn't return");
	struct mhdr *h = (struct mhdr *)(c->mem + p - 16);
	if (h->size & 0x80000000)
		cerr(c, c->line, "double free");
	h->size |= 0x80000000;
}

/* ---- declarations -------------------------------------------------------------------------- */

static struct type *parse_decl_spec(struct cint *c, int *is_typedef, int *is_static);
static struct type *parse_declarator(struct cint *c, struct type *base, char *name, int *line);
static struct node *parse_expr(struct cint *c);
static struct node *parse_assign(struct cint *c);
static struct node *parse_stmt(struct cint *c);
static long long const_eval(struct cint *c, struct node *n);

static int is_type_start(struct cint *c, struct tok *t)
{
	if (t->kind != K_ID)
		return 0;
	static const char *const kws[] = {
		"void", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
		"struct", "union", "enum", "const", "volatile", "static", "extern", "register",
		"typedef", "auto", "inline",
	};
	for (size_t i = 0; i < NELEM(kws); i++)
		if (!strcmp(t->s, kws[i]))
			return 1;
	struct sym *y = lookup(c, t->s);
	return y && y->kind == SY_TYPEDEF;
}

static struct type *parse_struct(struct cint *c)
{
	int line = peek(c)->line;
	char tag[32] = "";
	if (peek(c)->kind == K_ID)
		strncpy0(tag, next(c)->s, sizeof tag);
	struct type *t = tag[0] ? find_tag(c, tag) : NULL;
	if (!t || (is_p(c, "{") && t->complete && 0)) {
		t = calloc_c(c, sizeof *t);
		t->kind = TY_STRUCT;
		strncpy0(t->tag, tag, sizeof t->tag);
		if (tag[0] && c->scope->ntags < 32)
			c->scope->tags[c->scope->ntags++] = t;
	}
	if (!accept(c, "{"))
		return t;
	if (t->complete)
		cerr(c, line, "struct %s defined twice", tag);
	int off = 0, align = 1;
	struct member **mp = &t->members;
	while (!accept(c, "}")) {
		int td, st;
		struct type *base = parse_decl_spec(c, &td, &st);
		do {
			char name[64];
			int ml;
			struct type *mt = parse_declarator(c, base, name, &ml);
			if (!mt->complete)
				cerr(c, ml, "member %s has an incomplete type", name);
			off = (off + mt->align - 1) & ~(mt->align - 1);
			struct member *m = calloc_c(c, sizeof *m);
			strncpy0(m->name, name, sizeof m->name);
			m->ty = mt;
			m->offset = off;
			*mp = m;
			mp = &m->next;
			off += mt->size;
			align = MAX(align, mt->align);
		} while (accept(c, ","));
		expect(c, ";");
	}
	t->size = (off + align - 1) & ~(align - 1);
	t->align = align;
	t->complete = 1;
	return t;
}

static void parse_enum(struct cint *c)
{
	if (peek(c)->kind == K_ID)
		next(c);	/* tag: enums are just ints */
	if (!accept(c, "{"))
		return;
	long long v = 0;
	while (!accept(c, "}")) {
		struct tok *t = next(c);
		if (t->kind != K_ID)
			cerr(c, t->line, "expected an enumerator");
		if (accept(c, "="))
			v = const_eval(c, parse_assign(c));
		struct sym *y = add_sym(c, t->s, SY_ENUM, &ty_int);
		y->val = v++;
		if (!accept(c, ","))
			expect(c, "}"), c->pos--, accept(c, "}");
		if (c->toks[c->pos - 1].kind == K_P && c->toks[c->pos - 1].p == P("}"))
			break;
	}
}

static struct type *parse_decl_spec(struct cint *c, int *is_typedef, int *is_static)
{
	*is_typedef = *is_static = 0;
	int longs = 0, unsig = 0, seen = 0;
	struct type *t = NULL;
	for (;;) {
		struct tok *k = peek(c);
		if (k->kind != K_ID)
			break;
		const char *s = k->s;
		if (!strcmp(s, "const") || !strcmp(s, "volatile") || !strcmp(s, "register") ||
		    !strcmp(s, "auto") || !strcmp(s, "extern") || !strcmp(s, "inline") ||
		    !strcmp(s, "signed")) {
			next(c);
			seen = 1;
		} else if (!strcmp(s, "static")) {
			next(c);
			*is_static = 1;
		} else if (!strcmp(s, "typedef")) {
			next(c);
			*is_typedef = 1;
		} else if (!strcmp(s, "unsigned")) {
			next(c);
			unsig = 1;
			seen = 1;
		} else if (!strcmp(s, "void")) { next(c); t = &ty_void; }
		else if (!strcmp(s, "char")) { next(c); t = &ty_char; }
		else if (!strcmp(s, "short") || !strcmp(s, "int")) { next(c); if (!t) t = &ty_int; }
		else if (!strcmp(s, "long")) { next(c); longs++; }
		else if (!strcmp(s, "float") || !strcmp(s, "double")) { next(c); t = &ty_double; }
		else if (!strcmp(s, "struct") || !strcmp(s, "union")) {
			if (!strcmp(s, "union"))
				cerr(c, k->line, "unions aren't supported");
			next(c);
			t = parse_struct(c);
		} else if (!strcmp(s, "enum")) {
			next(c);
			parse_enum(c);
			t = &ty_int;
		} else {
			struct sym *y = lookup(c, s);
			if (y && y->kind == SY_TYPEDEF && !t && !longs) {
				next(c);
				t = y->ty;
			} else {
				break;
			}
		}
	}
	if (longs && (!t || t == &ty_int))
		t = &ty_long;
	if (!t) {
		if (!seen && !longs && !unsig)
			cerr(c, peek(c)->line, "expected a type");
		t = &ty_int;
	}
	if (unsig && is_int(t)) {
		struct type *u = calloc_c(c, sizeof *u);
		*u = *t;
		u->is_unsigned = 1;
		t = u;
	}
	return t;
}

static struct type *parse_abstract_suffix(struct cint *c, struct type *t)
{
	if (accept(c, "[")) {
		int len = -1;
		if (!is_p(c, "]"))
			len = (int)const_eval(c, parse_expr(c));
		expect(c, "]");
		t = parse_abstract_suffix(c, t);
		return array_of(c, t, len);
	}
	return t;
}

/* Declarators: pointers, (name), arrays and function parameter lists. */
static struct type *parse_declarator(struct cint *c, struct type *base, char *name, int *line)
{
	while (accept(c, "*")) {
		while (accept_id(c, "const") || accept_id(c, "volatile"))
			;
		base = ptr_to(c, base);
	}
	name[0] = 0;
	*line = peek(c)->line;
	if (accept(c, "(")) {
		/* (*name)(...) function pointers aren't supported; (name) is */
		if (is_p(c, "*"))
			cerr(c, *line, "function pointers aren't supported");
		struct type *t = parse_declarator(c, base, name, line);
		expect(c, ")");
		return t;
	}
	if (peek(c)->kind == K_ID && !is_type_start(c, peek(c)))
		strncpy0(name, next(c)->s, 64);
	if (is_p(c, "("))
		return base;	/* a function: the caller parses the parameters */
	return parse_abstract_suffix(c, base);
}

/* ---- expressions ------------------------------------------------------------------------- */

static struct type *decay(struct cint *c, struct type *t)
{
	if (t->kind == TY_ARRAY)
		return ptr_to(c, t->base);
	return t;
}

static struct type *arith_result(struct type *a, struct type *b)
{
	if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE)
		return &ty_double;
	if (a->kind == TY_LONG || b->kind == TY_LONG)
		return &ty_long;
	return &ty_int;
}

static struct node *num(struct cint *c, long long v, int line)
{
	struct node *n = new_node(c, N_NUM, line);
	n->ival = v;
	n->ty = &ty_int;
	if (v > 2147483647LL || v < -2147483648LL)
		n->ty = &ty_long;
	return n;
}

static struct node *parse_cast(struct cint *c);

static struct node *parse_primary(struct cint *c)
{
	struct tok *t = next(c);
	struct node *n;
	switch (t->kind) {
	case K_INT:
		n = num(c, t->ival, t->line);
		if (t->slen)
			n->ty = &ty_long;
		return n;
	case K_CHAR:
		n = num(c, t->ival, t->line);
		return n;
	case K_FLOAT:
		n = new_node(c, N_FNUM, t->line);
		n->fval = t->fval;
		n->ty = &ty_double;
		return n;
	case K_STR:
		n = new_node(c, N_STR, t->line);
		n->ival = intern_string(c, t->s, t->slen);
		n->ty = array_of(c, &ty_char, t->slen + 1);
		return n;
	case K_P:
		if (t->p == P("(")) {
			n = parse_expr(c);
			expect(c, ")");
			return n;
		}
		cerr(c, t->line, "expected an expression");
	case K_ID: {
		struct sym *y = lookup(c, t->s);
		if (is_p(c, "(") && (!y || y->kind == SY_FUNC)) {
			/* a call: user function, builtin, or SYS call - resolved after parsing */
			n = new_node(c, N_CALL, t->line);
			n->name = t->s;
			next(c);
			struct node **ap = &n->a;
			int nargs = 0;
			if (!accept(c, ")")) {
				do {
					*ap = parse_assign(c);
					ap = &(*ap)->next;
					nargs++;
				} while (accept(c, ","));
				expect(c, ")");
			}
			n->ival = nargs;
			struct func *f = find_func(c, t->s);
			n->fn = f;
			n->ty = f ? f->ret : &ty_long;	/* unknown: SYS calls return long */
			return n;
		}
		if (!y)
			cerr(c, t->line, "'%s' undeclared", t->s);
		if (y->kind == SY_ENUM)
			return num(c, y->val, t->line);
		if (y->kind == SY_TYPEDEF)
			cerr(c, t->line, "unexpected type name '%s'", t->s);
		if (y->kind == SY_FUNC)
			cerr(c, t->line, "function pointers aren't supported ('%s')", t->s);
		n = new_node(c, y->kind == SY_GVAR ? N_GVAR : N_VAR, t->line);
		n->offset = y->offset;
		n->ty = y->ty;
		n->name = t->s;
		return n;
	}
	}
	cerr(c, t->line, "expected an expression");
}

static struct node *index_node(struct cint *c, struct node *base, struct node *idx, int line);

static struct node *parse_postfix(struct cint *c)
{
	struct node *n = parse_primary(c);
	for (;;) {
		int line = peek(c)->line;
		if (accept(c, "[")) {
			struct node *i = parse_expr(c);
			expect(c, "]");
			n = index_node(c, n, i, line);
		} else if (is_p(c, ".") || is_p(c, "->")) {
			int arrow = is_p(c, "->");
			next(c);
			struct tok *m = next(c);
			if (m->kind != K_ID)
				cerr(c, m->line, "expected a member name");
			struct type *st = n->ty;
			if (arrow) {
				if (!is_ptr(st))
					cerr(c, line, "'->' needs a pointer to a struct");
				struct node *d = new_node(c, N_DEREF, line);
				d->a = n;
				d->ty = st->base;
				n = d;
				st = st->base;
			}
			if (st->kind != TY_STRUCT)
				cerr(c, line, "'%s' isn't a member of a struct", m->s);
			struct member *mm = st->members;
			while (mm && strcmp(mm->name, m->s))
				mm = mm->next;
			if (!mm)
				cerr(c, line, "struct %s has no member '%s'", st->tag, m->s);
			struct node *e = new_node(c, N_MEMBER, line);
			e->a = n;
			e->offset = mm->offset;
			e->ty = mm->ty;
			n = e;
		} else if (accept(c, "++") || accept(c, "--")) {
			struct node *e = new_node(c, c->toks[c->pos - 1].p == P("++") ? N_POSTINC : N_POSTDEC, line);
			e->a = n;
			e->ty = n->ty;
			n = e;
		} else {
			return n;
		}
	}
}

static struct node *index_node(struct cint *c, struct node *base, struct node *idx, int line)
{
	if (!is_ptr(base->ty) && is_ptr(idx->ty)) {	/* 3[a] */
		struct node *t = base;
		base = idx;
		idx = t;
	}
	if (!is_ptr(base->ty))
		cerr(c, line, "subscript of something that isn't an array or pointer");
	struct node *add = new_node(c, N_BIN, line);
	add->op = '+';
	add->a = base;
	add->b = idx;
	add->ty = decay(c, base->ty);
	struct node *d = new_node(c, N_DEREF, line);
	d->a = add;
	d->ty = base->ty->base;
	return d;
}

static int type_name_ahead(struct cint *c)
{
	return is_p(c, "(") && is_type_start(c, peekn(c, 1));
}

static struct type *parse_type_name(struct cint *c)
{
	int td, st;
	struct type *t = parse_decl_spec(c, &td, &st);
	while (accept(c, "*"))
		t = ptr_to(c, t);
	return parse_abstract_suffix(c, t);
}

static struct node *parse_unary(struct cint *c)
{
	int line = peek(c)->line;
	struct node *n;
	if (accept(c, "-")) {
		n = new_node(c, N_NEG, line);
		n->a = parse_cast(c);
		n->ty = n->a->ty->kind == TY_DOUBLE ? &ty_double : arith_result(n->a->ty, &ty_int);
		return n;
	}
	if (accept(c, "+"))
		return parse_cast(c);
	if (accept(c, "!")) {
		n = new_node(c, N_NOT, line);
		n->a = parse_cast(c);
		n->ty = &ty_int;
		return n;
	}
	if (accept(c, "~")) {
		n = new_node(c, N_BITNOT, line);
		n->a = parse_cast(c);
		n->ty = arith_result(n->a->ty, &ty_int);
		return n;
	}
	if (accept(c, "*")) {
		n = new_node(c, N_DEREF, line);
		n->a = parse_cast(c);
		if (!is_ptr(n->a->ty))
			cerr(c, line, "'*' needs a pointer");
		n->ty = n->a->ty->base;
		return n;
	}
	if (accept(c, "&")) {
		n = new_node(c, N_ADDR, line);
		n->a = parse_cast(c);
		n->ty = ptr_to(c, n->a->ty);
		return n;
	}
	if (accept(c, "++") || accept(c, "--")) {
		n = new_node(c, c->toks[c->pos - 1].p == P("++") ? N_PREINC : N_PREDEC, line);
		n->a = parse_unary(c);
		n->ty = n->a->ty;
		return n;
	}
	if (accept_id(c, "sizeof")) {
		struct type *t;
		if (type_name_ahead(c)) {
			next(c);
			t = parse_type_name(c);
			expect(c, ")");
		} else {
			t = parse_unary(c)->ty;
		}
		return num(c, t->size, line);
	}
	return parse_postfix(c);
}

static struct node *parse_cast(struct cint *c)
{
	if (type_name_ahead(c)) {
		int line = peek(c)->line;
		next(c);
		struct type *t = parse_type_name(c);
		expect(c, ")");
		struct node *n = new_node(c, N_CAST, line);
		n->a = parse_cast(c);
		n->ty = t;
		return n;
	}
	return parse_unary(c);
}

static struct node *binop(struct cint *c, int op, struct node *a, struct node *b, int line)
{
	struct node *n = new_node(c, N_BIN, line);
	n->op = op;
	n->a = a;
	n->b = b;
	struct type *ta = decay(c, a->ty), *tb = decay(c, b->ty);
	switch (op) {
	case '+':
		if (is_ptr(ta) && is_int(tb)) n->ty = ta;
		else if (is_int(ta) && is_ptr(tb)) n->ty = tb;
		else if (is_arith(ta) && is_arith(tb)) n->ty = arith_result(ta, tb);
		else cerr(c, line, "invalid operands to '+'");
		break;
	case '-':
		if (is_ptr(ta) && is_ptr(tb)) n->ty = &ty_long;
		else if (is_ptr(ta) && is_int(tb)) n->ty = ta;
		else if (is_arith(ta) && is_arith(tb)) n->ty = arith_result(ta, tb);
		else cerr(c, line, "invalid operands to '-'");
		break;
	case '*': case '/':
		if (!is_arith(ta) || !is_arith(tb))
			cerr(c, line, "invalid operands to '%c'", op);
		n->ty = arith_result(ta, tb);
		break;
	case '%': case '&': case '|': case '^': case 'l': case 'r':
		if (!is_int(ta) || !is_int(tb))
			cerr(c, line, "integer operands needed");
		n->ty = arith_result(ta, tb);
		break;
	default:	/* comparisons */
		n->ty = &ty_int;
	}
	return n;
}

struct binop_info {
	const char *tok;
	int op, prec;
};

static const struct binop_info binops[] = {
	{ "*", '*', 10 }, { "/", '/', 10 }, { "%", '%', 10 },
	{ "+", '+', 9 }, { "-", '-', 9 },
	{ "<<", 'l', 8 }, { ">>", 'r', 8 },
	{ "<", '<', 7 }, { ">", '>', 7 }, { "<=", 'L', 7 }, { ">=", 'G', 7 },
	{ "==", 'E', 6 }, { "!=", 'N', 6 },
	{ "&", '&', 5 }, { "^", '^', 4 }, { "|", '|', 3 },
	{ "&&", 'A', 2 }, { "||", 'O', 1 },
};

static struct node *parse_binary(struct cint *c, int min_prec)
{
	struct node *lhs = parse_cast(c);
	for (;;) {
		struct tok *t = peek(c);
		const struct binop_info *bi = NULL;
		if (t->kind == K_P)
			for (size_t i = 0; i < NELEM(binops); i++)
				if (t->p == P(binops[i].tok))
					bi = &binops[i];
		if (!bi || bi->prec < min_prec)
			return lhs;
		next(c);
		struct node *rhs = parse_binary(c, bi->prec + 1);
		if (bi->op == 'A' || bi->op == 'O') {
			struct node *n = new_node(c, bi->op == 'A' ? N_LOG_AND : N_LOG_OR, t->line);
			n->a = lhs;
			n->b = rhs;
			n->ty = &ty_int;
			lhs = n;
		} else {
			lhs = binop(c, bi->op, lhs, rhs, t->line);
		}
	}
}

static struct node *parse_cond(struct cint *c)
{
	struct node *n = parse_binary(c, 1);
	if (accept(c, "?")) {
		struct node *q = new_node(c, N_COND, n->line);
		q->a = n;
		q->b = parse_expr(c);
		expect(c, ":");
		q->c = parse_cond(c);
		q->ty = (q->b->ty->kind == TY_DOUBLE || q->c->ty->kind == TY_DOUBLE) ? &ty_double :
		    decay(c, q->b->ty);
		return q;
	}
	return n;
}

static struct node *parse_assign(struct cint *c)
{
	struct node *lhs = parse_cond(c);
	struct tok *t = peek(c);
	if (t->kind != K_P)
		return lhs;
	static const struct { const char *tok; int op; } aops[] = {
		{ "+=", '+' }, { "-=", '-' }, { "*=", '*' }, { "/=", '/' }, { "%=", '%' },
		{ "&=", '&' }, { "|=", '|' }, { "^=", '^' }, { "<<=", 'l' }, { ">>=", 'r' },
	};
	if (t->p == P("=")) {
		next(c);
		struct node *n = new_node(c, N_ASSIGN, t->line);
		n->a = lhs;
		n->b = parse_assign(c);
		n->ty = lhs->ty;
		if (lhs->ty->kind == TY_ARRAY)
			cerr(c, t->line, "can't assign to an array");
		return n;
	}
	for (size_t i = 0; i < NELEM(aops); i++) {
		if (t->p == P(aops[i].tok)) {
			next(c);
			struct node *n = new_node(c, N_OPASSIGN, t->line);
			n->a = lhs;
			n->b = parse_assign(c);
			n->op = aops[i].op;
			/* type-check through the equivalent binary expression */
			n->c = binop(c, aops[i].op, lhs, n->b, t->line);
			n->ty = lhs->ty;
			return n;
		}
	}
	return lhs;
}

static struct node *parse_expr(struct cint *c)
{
	struct node *n = parse_assign(c);
	while (accept(c, ",")) {
		struct node *m = new_node(c, N_COMMA, n->line);
		m->a = n;
		m->b = parse_assign(c);
		m->ty = m->b->ty;
		n = m;
	}
	return n;
}

static long long const_eval(struct cint *c, struct node *n)
{
	switch (n->kind) {
	case N_NUM: return n->ival;
	case N_FNUM: return (long long)n->fval;
	case N_NEG: return -const_eval(c, n->a);
	case N_BITNOT: return ~const_eval(c, n->a);
	case N_NOT: return !const_eval(c, n->a);
	case N_CAST: return const_eval(c, n->a);
	case N_BIN: {
		long long a = const_eval(c, n->a), b = const_eval(c, n->b);
		switch (n->op) {
		case '+': return a + b;
		case '-': return a - b;
		case '*': return a * b;
		case '/': if (!b) cerr(c, n->line, "division by zero"); return a / b;
		case '%': if (!b) cerr(c, n->line, "division by zero"); return a % b;
		case '&': return a & b;
		case '|': return a | b;
		case '^': return a ^ b;
		case 'l': return a << b;
		case 'r': return a >> b;
		case '<': return a < b;
		case '>': return a > b;
		case 'L': return a <= b;
		case 'G': return a >= b;
		case 'E': return a == b;
		case 'N': return a != b;
		}
	}
	}
	cerr(c, n->line, "a constant expression is needed here");
}

/* ---- statements and declarations ----------------------------------------------------- */

static struct node *parse_initializer(struct cint *c)
{
	if (accept(c, "{")) {
		struct node *n = new_node(c, N_ARRAYINIT, peek(c)->line);
		struct node **pp = &n->a;
		while (!accept(c, "}")) {
			*pp = parse_initializer(c);
			pp = &(*pp)->next;
			n->ival++;
			if (!accept(c, ",")) {
				expect(c, "}");
				break;
			}
		}
		return n;
	}
	return parse_assign(c);
}

/* Infer "int a[] = {1,2,3}" and "char s[] = "hi"" lengths. */
static struct type *fix_array_len(struct cint *c, struct type *t, struct node *init)
{
	if (t->kind != TY_ARRAY || t->len >= 0 || !init)
		return t;
	int len = init->kind == N_ARRAYINIT ? (int)init->ival :
	    init->kind == N_STR ? init->ty->len : -1;
	if (len < 0)
		return t;
	return array_of(c, t->base, len);
}

static int alloc_local(struct cint *c, struct type *t)
{
	int a = MAX(t->align, 1);
	c->frame_off = (c->frame_off + t->size + a - 1) & ~(a - 1);
	if (c->frame_off > c->max_frame)
		c->max_frame = c->frame_off;
	return -c->frame_off;
}

static struct node *parse_local_decl(struct cint *c)
{
	int td, st;
	int line = peek(c)->line;
	struct type *base = parse_decl_spec(c, &td, &st);
	struct node *first = NULL, **pp = &first;
	if (accept(c, ";"))
		return new_node(c, S_EMPTY, line);
	do {
		char name[64];
		int nl;
		struct type *t = parse_declarator(c, base, name, &nl);
		if (!name[0])
			cerr(c, nl, "expected a name");
		if (td) {
			add_sym(c, name, SY_TYPEDEF, t);
			continue;
		}
		if (is_p(c, "("))
			cerr(c, nl, "functions can't be declared inside functions");
		struct node *init = accept(c, "=") ? parse_initializer(c) : NULL;
		t = fix_array_len(c, t, init);
		if (!t->complete)
			cerr(c, nl, "'%s' has an incomplete type", name);
		struct node *d = new_node(c, S_DECL, nl);
		d->ty = t;
		d->init = init;
		if (st) {	/* static locals live with the globals */
			struct sym *y = add_sym(c, name, SY_GVAR, t);
			y->offset = (int)alloc_data(c, t->size, t->align);
			d->offset = y->offset;
			d->op = 1;	/* initialise once, at load */
			d->ival = 1;
		} else {
			struct sym *y = add_sym(c, name, SY_VAR, t);
			y->offset = alloc_local(c, t);
			d->offset = y->offset;
		}
		*pp = d;
		pp = &d->next;
	} while (accept(c, ","));
	expect(c, ";");
	if (!first)
		return new_node(c, S_EMPTY, line);
	if (!first->next)
		return first;
	struct node *blk = new_node(c, S_BLOCK, line);
	blk->a = first;
	blk->op = 1;	/* no new scope: just a list */
	return blk;
}

static struct node *parse_block(struct cint *c)
{
	struct node *b = new_node(c, S_BLOCK, peek(c)->line);
	expect(c, "{");
	push_scope(c);
	int save = c->frame_off;
	struct node **pp = &b->a;
	while (!accept(c, "}")) {
		if (peek(c)->kind == K_EOF)
			cerr(c, peek(c)->line, "missing '}'");
		*pp = is_type_start(c, peek(c)) ? parse_local_decl(c) : parse_stmt(c);
		pp = &(*pp)->next;
	}
	c->frame_off = save;
	pop_scope(c);
	return b;
}

static struct node *parse_stmt(struct cint *c)
{
	struct tok *t = peek(c);
	int line = t->line;
	struct node *n;
	if (is_p(c, "{"))
		return parse_block(c);
	if (accept(c, ";"))
		return new_node(c, S_EMPTY, line);
	if (accept_id(c, "if")) {
		n = new_node(c, S_IF, line);
		expect(c, "(");
		n->a = parse_expr(c);
		expect(c, ")");
		n->b = parse_stmt(c);
		if (accept_id(c, "else"))
			n->c = parse_stmt(c);
		return n;
	}
	if (accept_id(c, "while")) {
		n = new_node(c, S_WHILE, line);
		expect(c, "(");
		n->a = parse_expr(c);
		expect(c, ")");
		c->loop_depth++;
		n->b = parse_stmt(c);
		c->loop_depth--;
		return n;
	}
	if (accept_id(c, "do")) {
		n = new_node(c, S_DO, line);
		c->loop_depth++;
		n->b = parse_stmt(c);
		c->loop_depth--;
		if (!accept_id(c, "while"))
			cerr(c, peek(c)->line, "expected 'while'");
		expect(c, "(");
		n->a = parse_expr(c);
		expect(c, ")");
		expect(c, ";");
		return n;
	}
	if (accept_id(c, "for")) {
		n = new_node(c, S_FOR, line);
		expect(c, "(");
		push_scope(c);
		int save = c->frame_off;
		if (!accept(c, ";")) {
			if (is_type_start(c, peek(c)))
				n->d = parse_local_decl(c);
			else {
				n->d = new_node(c, S_EXPR, line);
				n->d->a = parse_expr(c);
				expect(c, ";");
			}
		}
		if (!is_p(c, ";"))
			n->a = parse_expr(c);
		expect(c, ";");
		if (!is_p(c, ")"))
			n->c = parse_expr(c);
		expect(c, ")");
		c->loop_depth++;
		n->b = parse_stmt(c);
		c->loop_depth--;
		c->frame_off = save;
		pop_scope(c);
		return n;
	}
	if (accept_id(c, "switch")) {
		n = new_node(c, S_SWITCH, line);
		expect(c, "(");
		n->a = parse_expr(c);
		expect(c, ")");
		c->switch_depth++;
		n->b = parse_stmt(c);
		c->switch_depth--;
		return n;
	}
	if (accept_id(c, "case")) {
		if (!c->switch_depth)
			cerr(c, line, "'case' outside a switch");
		n = new_node(c, S_CASE, line);
		n->ival = const_eval(c, parse_cond(c));
		expect(c, ":");
		return n;
	}
	if (accept_id(c, "default")) {
		if (!c->switch_depth)
			cerr(c, line, "'default' outside a switch");
		expect(c, ":");
		return new_node(c, S_DEFAULT, line);
	}
	if (accept_id(c, "break")) {
		if (!c->loop_depth && !c->switch_depth)
			cerr(c, line, "'break' outside a loop or switch");
		expect(c, ";");
		return new_node(c, S_BREAK, line);
	}
	if (accept_id(c, "continue")) {
		if (!c->loop_depth)
			cerr(c, line, "'continue' outside a loop");
		expect(c, ";");
		return new_node(c, S_CONTINUE, line);
	}
	if (accept_id(c, "return")) {
		n = new_node(c, S_RETURN, line);
		if (!is_p(c, ";"))
			n->a = parse_expr(c);
		expect(c, ";");
		return n;
	}
	if (accept_id(c, "goto"))
		cerr(c, line, "goto isn't supported");
	n = new_node(c, S_EXPR, line);
	n->a = parse_expr(c);
	expect(c, ";");
	return n;
}

/* ---- top level -------------------------------------------------------------------------- */

static void init_global(struct cint *c, uint32_t addr, struct type *t, struct node *init);

static void parse_function(struct cint *c, struct type *ret, const char *name, int line)
{
	struct func *f = find_func(c, name);
	if (!f) {
		f = calloc_c(c, sizeof *f);
		strncpy0(f->name, name, sizeof f->name);
		f->next = c->funcs;
		c->funcs = f;
		add_sym(c, name, SY_FUNC, ret)->fn = f;
	}
	f->ret = ret;
	expect(c, "(");
	push_scope(c);
	c->frame_off = 0;
	c->max_frame = 0;
	char pnames[16][64];
	int np = 0;
	if (is_id(c, "void") && peekn(c, 1)->kind == K_P && peekn(c, 1)->p == P(")")) {
		next(c);
	}
	while (!accept(c, ")")) {
		if (accept(c, "...")) {
			f->varargs = 1;
			expect(c, ")");
			break;
		}
		int td, st, pl;
		struct type *pt = parse_decl_spec(c, &td, &st);
		pt = parse_declarator(c, pt, pnames[np], &pl);
		if (pt->kind == TY_ARRAY)
			pt = ptr_to(c, pt->base);	/* array parameters are pointers */
		if (np >= 16)
			cerr(c, pl, "too many parameters");
		f->ptypes[np++] = pt;
		if (!accept(c, ","))
			expect(c, ")"), c->pos--, accept(c, ")");
		if (c->toks[c->pos - 1].p == P(")") && c->toks[c->pos - 1].kind == K_P)
			break;
	}
	f->nparams = np;
	if (accept(c, ";")) {	/* a prototype */
		pop_scope(c);
		return;
	}
	if (f->defined)
		cerr(c, line, "function '%s' defined twice", name);
	f->defined = 1;
	for (int i = 0; i < np; i++) {
		if (!pnames[i][0])
			cerr(c, line, "parameter %d of %s needs a name", i + 1, name);
		struct sym *y = add_sym(c, pnames[i], SY_VAR, f->ptypes[i]);
		y->offset = alloc_local(c, f->ptypes[i]);
		f->poffs[i] = y->offset;
	}
	struct func *save = c->cur_fn;
	c->cur_fn = f;
	f->body = parse_block(c);
	c->cur_fn = save;
	f->frame = (c->max_frame + 15) & ~15;
	pop_scope(c);
}

static void parse_toplevel(struct cint *c)
{
	while (peek(c)->kind != K_EOF) {
		if (accept(c, ";"))
			continue;
		int td, st;
		int line = peek(c)->line;
		struct type *base = parse_decl_spec(c, &td, &st);
		if (accept(c, ";"))
			continue;	/* struct or enum declaration */
		for (;;) {
			char name[64];
			int nl;
			struct type *t = parse_declarator(c, base, name, &nl);
			if (!name[0])
				cerr(c, nl, "expected a name");
			if (is_p(c, "(")) {
				parse_function(c, t, name, line);
				if (c->toks[c->pos - 1].kind == K_P && c->toks[c->pos - 1].p == P("}"))
					goto next_decl;
				break;	/* prototype consumed its ';' */
			}
			if (td) {
				add_sym(c, name, SY_TYPEDEF, t);
			} else {
				struct node *init = accept(c, "=") ? parse_initializer(c) : NULL;
				t = fix_array_len(c, t, init);
				if (!t->complete)
					cerr(c, nl, "'%s' has an incomplete type", name);
				struct sym *y = add_sym(c, name, SY_GVAR, t);
				y->offset = (int)alloc_data(c, t->size, t->align);
				if (init)
					init_global(c, (uint32_t)y->offset, t, init);
			}
			if (accept(c, ","))
				continue;
			expect(c, ";");
			break;
		}
next_decl:;
	}
}

/* ---- evaluation --------------------------------------------------------------------------- */

struct val {
	long long i;
	double d;
};

static struct val eval(struct cint *c, struct node *n);
static uint64_t lvalue(struct cint *c, struct node *n);

static struct val load(struct cint *c, struct type *t, uint64_t a)
{
	struct val v = { 0, 0 };
	if (t->kind == TY_ARRAY || t->kind == TY_STRUCT) {
		v.i = (long long)a;	/* arrays decay; structs are handled by address */
		return v;
	}
	check_addr(c, a, t->size);
	uint8_t *p = c->mem + a;
	switch (t->kind) {
	case TY_CHAR: v.i = t->is_unsigned ? (long long)*p : (long long)(int8_t)*p; break;
	case TY_INT: {
		int32_t x;
		memcpy(&x, p, 4);
		v.i = t->is_unsigned ? (long long)(uint32_t)x : x;
		break;
	}
	case TY_LONG:
	case TY_PTR:
		memcpy(&v.i, p, 8);
		break;
	case TY_DOUBLE:
		memcpy(&v.d, p, 8);
		break;
	}
	return v;
}

static struct val convert(struct type *from, struct type *to, struct val v)
{
	struct val r = v;
	if (to->kind == TY_DOUBLE) {
		if (from->kind != TY_DOUBLE)
			r.d = from->is_unsigned ? (double)(unsigned long long)v.i : (double)v.i;
	} else if (from->kind == TY_DOUBLE) {
		r.i = (long long)v.d;
	}
	switch (to->kind) {
	case TY_CHAR: r.i = to->is_unsigned ? (long long)(uint8_t)r.i : (long long)(int8_t)r.i; break;
	case TY_INT: r.i = to->is_unsigned ? (long long)(uint32_t)r.i : (long long)(int32_t)r.i; break;
	}
	return r;
}

static void store(struct cint *c, struct type *t, uint64_t a, struct val v)
{
	check_addr(c, a, t->size);
	uint8_t *p = c->mem + a;
	switch (t->kind) {
	case TY_CHAR: *p = (uint8_t)v.i; break;
	case TY_INT: {
		int32_t x = (int32_t)v.i;
		memcpy(p, &x, 4);
		break;
	}
	case TY_LONG:
	case TY_PTR:
		memcpy(p, &v.i, 8);
		break;
	case TY_DOUBLE:
		memcpy(p, &v.d, 8);
		break;
	default:
		cerr(c, c->line, "can't store a value of this type");
	}
}

static uint64_t lvalue(struct cint *c, struct node *n)
{
	switch (n->kind) {
	case N_VAR: return c->fp + (uint64_t)(int64_t)n->offset;
	case N_GVAR: return (uint64_t)(uint32_t)n->offset;
	case N_DEREF: {
		struct val v = eval(c, n->a);
		return (uint64_t)v.i;
	}
	case N_MEMBER:
		return lvalue(c, n->a) + (uint64_t)n->offset;
	case N_STR:
		return (uint64_t)n->ival;
	case N_CALL:
		if (n->ty->kind == TY_STRUCT)
			return (uint64_t)eval(c, n).i;
		break;
	}
	cerr(c, n->line, "this can't be assigned to or have its address taken");
}

static int truthy(struct node *n, struct val v)
{
	return n->ty->kind == TY_DOUBLE ? v.d != 0 : v.i != 0;
}

static struct val arith(struct cint *c, int op, struct type *rt, struct node *an, struct val a,
    struct node *bn, struct val b, int line)
{
	struct type *ta = decay(c, an->ty), *tb = decay(c, bn->ty);
	struct val r = { 0, 0 };
	/* pointer arithmetic */
	if (op == '+' || op == '-') {
		if (is_ptr(ta) && is_int(tb)) {
			r.i = a.i + (op == '+' ? 1 : -1) * b.i * ta->base->size;
			return r;
		}
		if (is_int(ta) && is_ptr(tb)) {
			r.i = b.i + a.i * tb->base->size;
			return r;
		}
		if (is_ptr(ta) && is_ptr(tb)) {
			r.i = (a.i - b.i) / MAX(ta->base->size, 1);
			return r;
		}
	}
	int cmp = op == '<' || op == '>' || op == 'L' || op == 'G' || op == 'E' || op == 'N';
	if ((cmp && (ta->kind == TY_DOUBLE || tb->kind == TY_DOUBLE)) || rt->kind == TY_DOUBLE) {
		double x = convert(ta, &ty_double, a).d, y = convert(tb, &ty_double, b).d;
		switch (op) {
		case '+': r.d = x + y; break;
		case '-': r.d = x - y; break;
		case '*': r.d = x * y; break;
		case '/': r.d = x / y; break;
		case '<': r.i = x < y; break;
		case '>': r.i = x > y; break;
		case 'L': r.i = x <= y; break;
		case 'G': r.i = x >= y; break;
		case 'E': r.i = x == y; break;
		case 'N': r.i = x != y; break;
		}
		return r;
	}
	long long x = a.i, y = b.i;
	int uns = ta->is_unsigned || tb->is_unsigned;
	switch (op) {
	case '+': r.i = x + y; break;
	case '-': r.i = x - y; break;
	case '*': r.i = x * y; break;
	case '/':
	case '%':
		if (!y)
			cerr(c, line, "division by zero");
		if (uns)
			r.i = (long long)(op == '/' ? (unsigned long long)x / (unsigned long long)y :
			    (unsigned long long)x % (unsigned long long)y);
		else
			r.i = op == '/' ? x / y : x % y;
		break;
	case '&': r.i = x & y; break;
	case '|': r.i = x | y; break;
	case '^': r.i = x ^ y; break;
	case 'l': r.i = (long long)((unsigned long long)x << (y & 63)); break;
	case 'r': r.i = uns ? (long long)((unsigned long long)x >> (y & 63)) : x >> (y & 63); break;
	case '<': r.i = uns ? (unsigned long long)x < (unsigned long long)y : x < y; break;
	case '>': r.i = uns ? (unsigned long long)x > (unsigned long long)y : x > y; break;
	case 'L': r.i = uns ? (unsigned long long)x <= (unsigned long long)y : x <= y; break;
	case 'G': r.i = uns ? (unsigned long long)x >= (unsigned long long)y : x >= y; break;
	case 'E': r.i = x == y; break;
	case 'N': r.i = x != y; break;
	}
	return convert(&ty_long, rt, r);
}

static struct val call(struct cint *c, struct node *n);

static struct val incdec(struct cint *c, struct node *n, int delta, int post)
{
	uint64_t a = lvalue(c, n->a);
	struct type *t = n->a->ty;
	struct val old = load(c, t, a), nv = old;
	if (t->kind == TY_DOUBLE)
		nv.d += delta;
	else if (t->kind == TY_PTR)
		nv.i += delta * t->base->size;
	else
		nv = convert(&ty_long, t, (struct val){ old.i + delta, 0 });
	store(c, t, a, nv);
	return post ? old : nv;
}

static struct val eval(struct cint *c, struct node *n)
{
	struct val v = { 0, 0 };
	c->line = n->line;
	switch (n->kind) {
	case N_NUM: v.i = n->ival; return v;
	case N_FNUM: v.d = n->fval; return v;
	case N_STR: v.i = n->ival; return v;
	case N_VAR:
	case N_GVAR:
	case N_MEMBER:
		return load(c, n->ty, lvalue(c, n));
	case N_DEREF: {
		struct val p = eval(c, n->a);
		if (n->ty->kind == TY_FUNC || n->ty->kind == TY_VOID)
			cerr(c, n->line, "can't dereference that");
		return load(c, n->ty, (uint64_t)p.i);
	}
	case N_ADDR:
		v.i = (long long)lvalue(c, n->a);
		return v;
	case N_NEG: {
		struct val a = eval(c, n->a);
		if (n->ty->kind == TY_DOUBLE) {
			v.d = -convert(n->a->ty, &ty_double, a).d;
		} else {
			v.i = -a.i;
			v = convert(&ty_long, n->ty, v);
		}
		return v;
	}
	case N_NOT:
		v.i = !truthy(n->a, eval(c, n->a));
		return v;
	case N_BITNOT:
		v.i = ~eval(c, n->a).i;
		return convert(&ty_long, n->ty, v);
	case N_CAST: {
		struct val a = eval(c, n->a);
		if (n->ty->kind == TY_VOID)
			return v;
		return convert(decay(c, n->a->ty), n->ty, a);
	}
	case N_BIN: {
		struct val a = eval(c, n->a), b = eval(c, n->b);
		return arith(c, n->op, n->ty, n->a, a, n->b, b, n->line);
	}
	case N_LOG_AND:
		v.i = truthy(n->a, eval(c, n->a)) && truthy(n->b, eval(c, n->b));
		return v;
	case N_LOG_OR:
		v.i = truthy(n->a, eval(c, n->a)) || truthy(n->b, eval(c, n->b));
		return v;
	case N_COND: {
		struct val q = eval(c, n->a);
		struct node *pick = truthy(n->a, q) ? n->b : n->c;
		return convert(decay(c, pick->ty), n->ty, eval(c, pick));
	}
	case N_COMMA:
		eval(c, n->a);
		return eval(c, n->b);
	case N_ASSIGN: {
		uint64_t a = lvalue(c, n->a);
		if (n->a->ty->kind == TY_STRUCT) {
			uint64_t src = (uint64_t)eval(c, n->b).i;
			check_addr(c, src, n->a->ty->size);
			check_addr(c, a, n->a->ty->size);
			memmove(c->mem + a, c->mem + src, (size_t)n->a->ty->size);
			v.i = (long long)a;
			return v;
		}
		struct val b = convert(decay(c, n->b->ty), n->a->ty, eval(c, n->b));
		store(c, n->a->ty, a, b);
		return b;
	}
	case N_OPASSIGN: {
		uint64_t a = lvalue(c, n->a);
		struct val cur = load(c, n->a->ty, a), b = eval(c, n->b);
		struct val r = arith(c, n->op, n->c->ty, n->a, cur, n->b, b, n->line);
		r = convert(n->c->ty, n->a->ty, r);
		store(c, n->a->ty, a, r);
		return r;
	}
	case N_PREINC: return incdec(c, n, 1, 0);
	case N_PREDEC: return incdec(c, n, -1, 0);
	case N_POSTINC: return incdec(c, n, 1, 1);
	case N_POSTDEC: return incdec(c, n, -1, 1);
	case N_CALL:
		return call(c, n);
	}
	cerr(c, n->line, "can't evaluate this expression");
}

/* ---- statements ------------------------------------------------------------------------------ */

enum { X_NORMAL, X_BREAK, X_CONTINUE, X_RETURN };

static void housekeeping(struct cint *c)
{
	if (++c->steps & 4095)
		return;
	int k = con_pollkey();
	if (k == 3 || k == KEY_EOF)
		cerr(c, c->line, "stopped (Ctrl-C)");
	task_yield();
}

static void init_local(struct cint *c, uint64_t addr, struct type *t, struct node *init)
{
	if (!init) {
		return;
	}
	if (t->kind == TY_ARRAY) {
		check_addr(c, addr, t->size);
		memset(c->mem + addr, 0, (size_t)t->size);
		if (init->kind == N_STR && t->base->kind == TY_CHAR) {
			int len = MIN(init->ty->len, t->len);
			memcpy(c->mem + addr, c->mem + init->ival, (size_t)len);
			return;
		}
		if (init->kind != N_ARRAYINIT)
			cerr(c, init->line, "arrays need a { ... } initialiser");
		int i = 0;
		for (struct node *e = init->a; e && i < t->len; e = e->next, i++)
			init_local(c, addr + (uint64_t)(i * t->base->size), t->base, e);
		return;
	}
	if (t->kind == TY_STRUCT) {
		check_addr(c, addr, t->size);
		if (init->kind != N_ARRAYINIT) {
			uint64_t src = (uint64_t)eval(c, init).i;
			memmove(c->mem + addr, c->mem + src, (size_t)t->size);
			return;
		}
		memset(c->mem + addr, 0, (size_t)t->size);
		struct member *m = t->members;
		for (struct node *e = init->a; e && m; e = e->next, m = m->next)
			init_local(c, addr + (uint64_t)m->offset, m->ty, e);
		return;
	}
	if (init->kind == N_ARRAYINIT)
		init = init->a;
	store(c, t, addr, convert(decay(c, init->ty), t, eval(c, init)));
}

static void init_global(struct cint *c, uint32_t addr, struct type *t, struct node *init)
{
	/* globals are initialised by evaluating the expressions at load time */
	uint32_t save_fp = c->fp;
	init_local(c, addr, t, init);
	c->fp = save_fp;
}

static int exec(struct cint *c, struct node *n);

/* a switch body: find the matching case, then run from there */
static int exec_switch(struct cint *c, struct node *n)
{
	long long v = eval(c, n->a).i;
	struct node *body = n->b;
	if (body->kind != S_BLOCK)
		return exec(c, body) == X_RETURN ? X_RETURN : X_NORMAL;
	struct node *start = NULL, *def = NULL;
	for (struct node *s = body->a; s; s = s->next) {
		if (s->kind == S_CASE && s->ival == v && !start)
			start = s;
		if (s->kind == S_DEFAULT)
			def = s;
	}
	if (!start)
		start = def;
	for (struct node *s = start; s; s = s->next) {
		int r = exec(c, s);
		if (r == X_BREAK)
			return X_NORMAL;
		if (r != X_NORMAL)
			return r;
	}
	return X_NORMAL;
}

static int exec(struct cint *c, struct node *n)
{
	housekeeping(c);
	c->line = n->line;
	switch (n->kind) {
	case S_EMPTY:
	case S_CASE:
	case S_DEFAULT:
		return X_NORMAL;
	case S_EXPR:
		eval(c, n->a);
		return X_NORMAL;
	case S_DECL:
		if (n->op == 1)	/* static local: done at load time */
			return X_NORMAL;
		init_local(c, c->fp + (uint64_t)(int64_t)n->offset, n->ty, n->init);
		return X_NORMAL;
	case S_BLOCK:
		for (struct node *s = n->a; s; s = s->next) {
			int r = exec(c, s);
			if (r != X_NORMAL)
				return r;
		}
		return X_NORMAL;
	case S_IF:
		if (truthy(n->a, eval(c, n->a)))
			return exec(c, n->b);
		return n->c ? exec(c, n->c) : X_NORMAL;
	case S_WHILE:
		while (truthy(n->a, eval(c, n->a))) {
			int r = exec(c, n->b);
			if (r == X_BREAK)
				break;
			if (r == X_RETURN)
				return r;
		}
		return X_NORMAL;
	case S_DO:
		do {
			int r = exec(c, n->b);
			if (r == X_BREAK)
				break;
			if (r == X_RETURN)
				return r;
		} while (truthy(n->a, eval(c, n->a)));
		return X_NORMAL;
	case S_FOR:
		if (n->d)
			exec(c, n->d);
		for (;;) {
			if (n->a && !truthy(n->a, eval(c, n->a)))
				break;
			int r = exec(c, n->b);
			if (r == X_BREAK)
				break;
			if (r == X_RETURN)
				return r;
			if (n->c)
				eval(c, n->c);
		}
		return X_NORMAL;
	case S_SWITCH:
		return exec_switch(c, n);
	case S_BREAK:
		return X_BREAK;
	case S_CONTINUE:
		return X_CONTINUE;
	case S_RETURN:
		if (n->a) {
			struct val v = eval(c, n->a);
			struct type *rt = c->ret_ty;
			if (rt && rt->kind == TY_STRUCT) {
				c->ret_i = v.i;	/* address; the caller copies it */
			} else if (rt && rt->kind != TY_VOID) {
				v = convert(decay(c, n->a->ty), rt, v);
				c->ret_i = v.i;
				c->ret_d = v.d;
			}
		}
		return X_RETURN;
	}
	cerr(c, n->line, "can't run this statement");
}

/* ---- calls and the library ---------------------------------------------------------------- */

static int format_into(struct cint *c, char *out, size_t cap, uint64_t fmt_addr, struct val *args,
    struct node **argn, int nargs);

static const char *cstr(struct cint *c, uint64_t a)
{
	check_addr(c, a, 1);
	/* make sure it ends inside memory */
	uint64_t e = a;
	while (e < MEMSZ && c->mem[e])
		e++;
	if (e >= MEMSZ)
		cerr(c, c->line, "string runs off the end of memory");
	return (const char *)c->mem + a;
}

static void out_char(struct cint *c, char ch)
{
	con_putc(ch);
	c->col = ch == '\n' ? 0 : c->col + 1;
}

static void out_str(struct cint *c, const char *s)
{
	while (*s)
		out_char(c, *s++);
}

static int read_line_into(struct cint *c, uint64_t buf, int n)
{
	char line[512];
	int len = read_line(line, sizeof line);
	if (len < 0)
		return -1;
	len = MIN(len, n - 1);
	check_addr(c, buf, len + 1);
	memcpy(c->mem + buf, line, (size_t)len);
	c->mem[buf + (uint64_t)len] = 0;
	return len;
}

/* Turn a program-level name like win_wait_close into a SYS call name:
 * underscores and case don't matter, so win_open, WinOpen and Win_Open all
 * reach "Win_Open". */
struct sys_match {
	const char *want;
	const struct sys_entry *hit;
};

static void sys_match_cb(const struct sys_entry *e, void *arg)
{
	struct sys_match *m = arg;
	const char *a = e->name, *b = m->want;
	for (;;) {
		while (*a == '_')
			a++;
		while (*b == '_')
			b++;
		if (tolower(*a) != tolower(*b))
			return;
		if (!*a)
			break;
		a++;
		b++;
	}
	m->hit = e;
}

static const struct sys_entry *find_sys(const char *name)
{
	struct sys_match m = { name, NULL };
	sys_list(sys_match_cb, &m);
	return m.hit;
}

static int builtin(struct cint *c, struct node *n, struct val *a, struct node **an, int na,
    struct val *r);

static struct val call(struct cint *c, struct node *n)
{
	struct val args[16];
	struct node *argn[16];
	int na = 0;
	for (struct node *e = n->a; e; e = e->next) {
		if (na >= 16)
			cerr(c, n->line, "too many arguments");
		argn[na] = e;
		args[na++] = eval(c, e);
	}
	c->line = n->line;
	struct func *f = n->fn;
	if (!f || !f->defined)	/* called before it was defined */
		f = n->fn = find_func(c, n->name);
	struct val r = { 0, 0 };
	if (f && f->defined) {
		if (na != f->nparams && !f->varargs)
			cerr(c, n->line, "%s() takes %d argument%s, not %d", f->name, f->nparams,
			    f->nparams == 1 ? "" : "s", na);
		if (++c->depth > MAX_DEPTH)
			cerr(c, n->line, "too much recursion");
		uint32_t frame = (uint32_t)f->frame;
		if (c->sp - frame < c->heap_end + 64)
			cerr(c, n->line, "stack overflow");
		uint32_t save_fp = c->fp, save_sp = c->sp;
		struct type *save_rt = c->ret_ty;
		c->sp -= frame;
		c->sp &= ~15u;
		uint32_t new_fp = c->sp + frame;
		memset(c->mem + c->sp, 0, frame);
		for (int i = 0; i < f->nparams; i++) {
			struct type *pt = f->ptypes[i];
			struct val v = pt->kind == TY_STRUCT ? args[i] :
			    convert(decay(c, argn[i]->ty), pt, args[i]);
			uint64_t pa = new_fp + (uint64_t)(int64_t)f->poffs[i];
			if (pt->kind == TY_STRUCT) {
				check_addr(c, (uint64_t)v.i, pt->size);
				memmove(c->mem + pa, c->mem + v.i, (size_t)pt->size);
			} else {
				store(c, pt, pa, v);
			}
		}
		c->fp = new_fp;
		c->ret_i = 0;
		c->ret_d = 0;
		c->ret_ty = f->ret;
		exec(c, f->body);
		r.i = c->ret_i;
		r.d = c->ret_d;
		if (f->ret->kind == TY_STRUCT) {
			/* copy the struct out of the dying frame into a temporary */
			uint32_t tmp = c_malloc(c, (uint64_t)f->ret->size);
			if (!tmp)
				cerr(c, n->line, "out of memory");
			memmove(c->mem + tmp, c->mem + r.i, (size_t)f->ret->size);
			r.i = tmp;
		}
		c->fp = save_fp;
		c->sp = save_sp;
		c->ret_ty = save_rt;
		c->depth--;
		return r;
	}
	if (builtin(c, n, args, argn, na, &r))
		return r;
	/* a SYS call: win_open(...) -> SYS "Win_Open" */
	const struct sys_entry *e = find_sys(n->name);
	if (!e) {
		if (f)
			cerr(c, n->line, "%s() is declared but never defined", n->name);
		cerr(c, n->line, "%s() isn't defined (and isn't a SYS call)", n->name);
	}
	struct sysval sv[SYS_MAX_ARGS], ret;
	for (int i = 0; i < na && i < SYS_MAX_ARGS; i++) {
		memset(&sv[i], 0, sizeof sv[i]);
		struct type *t = decay(c, argn[i]->ty);
		if (t->kind == TY_PTR && t->base->kind == TY_CHAR) {
			sv[i].is_str = 1;
			strncpy0(sv[i].s, cstr(c, (uint64_t)args[i].i), sizeof sv[i].s);
		} else if (t->kind == TY_DOUBLE) {
			sv[i].n = (long long)args[i].d;
			sv[i].f = args[i].d;
		} else {
			sv[i].n = args[i].i;
			sv[i].f = (double)args[i].i;
		}
	}
	int rc = sys_call(e->name, MIN(na, SYS_MAX_ARGS), sv, &ret);
	if (rc < 0)
		cerr(c, n->line, "%s() failed%s", n->name, rc == -2 && !task_current()->desk &&
		    !strncasecmp(e->name, "Win", 3) ? " (start the desktop with WIN first)" :
		    rc == -2 && !strncasecmp(e->name, "Desk", 4) ? " (start the desktop with WIN first)" : "");
	if (ret.is_str) {
		uint32_t s = c_malloc(c, strlen(ret.s) + 1);
		if (s)
			memcpy(c->mem + s, ret.s, strlen(ret.s) + 1);
		r.i = s;
	} else {
		r.i = ret.n;
		r.d = (double)ret.n;
	}
	return r;
}

/* printf-style formatting into a buffer */
static int format_into(struct cint *c, char *out, size_t cap, uint64_t fmt_addr, struct val *args,
    struct node **argn, int nargs)
{
	const char *f = cstr(c, fmt_addr);
	size_t o = 0;
	int ai = 0;
#define PUT(ch) do { if (o + 1 < cap) out[o] = (ch); o++; } while (0)
	for (; *f; f++) {
		if (*f != '%') {
			PUT(*f);
			continue;
		}
		f++;
		if (*f == '%') {
			PUT('%');
			continue;
		}
		int left = 0, zero = 0, plus = 0, space = 0, width = 0, prec = -1;
		for (;; f++) {
			if (*f == '-') left = 1;
			else if (*f == '0') zero = 1;
			else if (*f == '+') plus = 1;
			else if (*f == ' ') space = 1;
			else if (*f == '#') ;
			else break;
		}
		if (*f == '*') {
			width = ai < nargs ? (int)args[ai++].i : 0;
			f++;
		}
		while (isdigit((unsigned char)*f))
			width = width * 10 + (*f++ - '0');
		if (*f == '.') {
			f++;
			prec = 0;
			if (*f == '*') {
				prec = ai < nargs ? (int)args[ai++].i : 0;
				f++;
			}
			while (isdigit((unsigned char)*f))
				prec = prec * 10 + (*f++ - '0');
		}
		while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'L')
			f++;
		char conv = *f;
		if (!conv)
			break;
		char buf[400];
		int bl = 0;
		struct val v = ai < nargs ? args[ai] : (struct val){ 0, 0 };
		struct type *vt = ai < nargs ? decay(c, argn[ai]->ty) : &ty_int;
		ai++;
		switch (conv) {
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': case 'p': {
			long long x = vt->kind == TY_DOUBLE ? (long long)v.d : v.i;
			if (conv == 'c') {
				buf[bl++] = (char)x;
				break;
			}
			if (conv == 'p') {
				bl = snprintf(buf, sizeof buf, "%lx", (unsigned long long)x);
				break;
			}
			int neg = 0;
			unsigned long long u;
			int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : 10;
			if ((conv == 'd' || conv == 'i') && x < 0) {
				neg = 1;
				u = (unsigned long long)(-x);
			} else if (conv == 'u' || base != 10) {
				u = vt->kind == TY_INT || vt->kind == TY_CHAR ? (uint32_t)x : (unsigned long long)x;
			} else {
				u = (unsigned long long)x;
			}
			char d[24];
			int nd = 0;
			const char *digits = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
			do {
				d[nd++] = digits[u % (unsigned)base];
				u /= (unsigned)base;
			} while (u);
			while (nd < prec)
				d[nd++] = '0';
			if (neg) buf[bl++] = '-';
			else if (plus) buf[bl++] = '+';
			else if (space) buf[bl++] = ' ';
			while (nd)
				buf[bl++] = d[--nd];
			break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
			double x = vt->kind == TY_DOUBLE ? v.d : (double)v.i;
			if (prec < 0)
				prec = 6;
			if (x < 0) {
				buf[bl++] = '-';
				x = -x;
			} else if (plus) {
				buf[bl++] = '+';
			}
			if (conv == 'g' || conv == 'G') {
				bl += f_format(x, buf + bl, (int)sizeof buf - bl, prec ? prec : 1);
				break;
			}
			if (conv == 'e' || conv == 'E') {
				int e = 0;
				if (x != 0) {
					while (x >= 10) { x /= 10; e++; }
					while (x < 1) { x *= 10; e--; }
				}
				double rnd = 0.5;
				for (int i = 0; i < prec; i++) rnd /= 10;
				x += rnd;
				if (x >= 10) { x /= 10; e++; }
				int ip = (int)x;
				buf[bl++] = (char)('0' + ip);
				x -= ip;
				if (prec) buf[bl++] = '.';
				for (int i = 0; i < prec; i++) {
					x *= 10;
					int dg = (int)x;
					buf[bl++] = (char)('0' + dg);
					x -= dg;
				}
				bl += snprintf(buf + bl, sizeof buf - (size_t)bl, "%c%c%02d", conv, e < 0 ? '-' : '+',
				    e < 0 ? -e : e);
				break;
			}
			if (x >= 1e18) {
				bl += f_format(x, buf + bl, (int)sizeof buf - bl, 15);
				break;
			}
			double rnd = 0.5;
			for (int i = 0; i < prec; i++)
				rnd /= 10;
			x += rnd;
			unsigned long long ip = (unsigned long long)x;
			double frac = x - (double)ip;
			char d[24];
			int nd = 0;
			do {
				d[nd++] = (char)('0' + ip % 10);
				ip /= 10;
			} while (ip);
			while (nd)
				buf[bl++] = d[--nd];
			if (prec) {
				buf[bl++] = '.';
				for (int i = 0; i < prec && bl < (int)sizeof buf - 2; i++) {
					frac *= 10;
					int dg = (int)frac;
					if (dg > 9) dg = 9;
					buf[bl++] = (char)('0' + dg);
					frac -= dg;
				}
			}
			break;
		}
		case 's': {
			const char *s = v.i ? cstr(c, (uint64_t)v.i) : "(null)";
			int l = (int)strlen(s);
			if (prec >= 0 && l > prec)
				l = prec;
			l = MIN(l, (int)sizeof buf - 1);
			memcpy(buf, s, (size_t)l);
			bl = l;
			break;
		}
		default:
			buf[bl++] = '%';
			buf[bl++] = conv;
		}
		int pad = width - bl;
		if (!left)
			while (pad-- > 0)
				PUT(zero && conv != 's' && conv != 'c' ? '0' : ' ');
		for (int i = 0; i < bl; i++)
			PUT(buf[i]);
		if (left)
			while (pad-- > 0)
				PUT(' ');
	}
#undef PUT
	if (cap)
		out[o < cap ? o : cap - 1] = 0;
	return (int)o;
}

static int get_key_c(void)
{
	return con_getkey();
}

/* The C library. Returns 1 if name was a builtin. */
static int builtin(struct cint *c, struct node *n, struct val *a, struct node **an, int na,
    struct val *r)
{
	const char *name = n->name;
	#define IS(s) (!strcmp(name, s))
	#define NEED(k) do { if (na < (k)) cerr(c, n->line, "%s() needs %d argument%s", name, k, (k) == 1 ? "" : "s"); } while (0)
	#define ARGD(k) (decay(c, an[k]->ty)->kind == TY_DOUBLE ? a[k].d : (double)a[k].i)
	r->i = 0;
	r->d = 0;
	if (IS("printf") || IS("sprintf") || IS("snprintf")) {
		int base = IS("printf") ? 0 : IS("sprintf") ? 1 : 2;
		NEED(base + 1);
		char *buf = kmalloc(8192);
		if (!buf)
			cerr(c, n->line, "out of memory");
		int len = format_into(c, buf, 8192, (uint64_t)a[base].i, a + base + 1, an + base + 1,
		    na - base - 1);
		if (!base) {
			out_str(c, buf);
		} else {
			size_t cap = base == 2 ? (size_t)a[1].i : 8192;
			if (cap) {
				size_t l = MIN((size_t)len, cap - 1);
				check_addr(c, (uint64_t)a[0].i, (int)l + 1);
				memcpy(c->mem + a[0].i, buf, l);
				c->mem[a[0].i + (long long)l] = 0;
			}
		}
		kfree(buf);
		r->i = len;
		return 1;
	}
	if (IS("puts")) {
		NEED(1);
		out_str(c, cstr(c, (uint64_t)a[0].i));
		out_char(c, '\n');
		return 1;
	}
	if (IS("putchar") || IS("putch")) {
		NEED(1);
		out_char(c, (char)a[0].i);
		r->i = a[0].i;
		return 1;
	}
	if (IS("getchar")) {
		/* line-buffered, like a real terminal */
		static char lbuf[512];
		static int lpos = -1, llen;
		if (lpos < 0 || lpos > llen) {
			llen = read_line(lbuf, sizeof lbuf - 1);
			if (llen < 0) {
				lpos = -1;
				r->i = -1;
				return 1;
			}
			lbuf[llen] = '\n';
			lpos = 0;
		}
		r->i = (unsigned char)lbuf[lpos++];
		if (lpos > llen)
			lpos = -1;
		return 1;
	}
	if (IS("getch")) {	/* conio: a key, no echo */
		int k = get_key_c();
		r->i = k == '\n' ? '\r' : k;
		return 1;
	}
	if (IS("getche")) {
		int k = get_key_c();
		out_char(c, (char)k);
		r->i = k;
		return 1;
	}
	if (IS("kbhit")) {
		int k = con_pollkey();
		if (k >= 0)
			input_key(k);	/* put it back for getch */
		r->i = k >= 0;
		if (k < 0)
			task_idle();
		return 1;
	}
	if (IS("gets") || IS("fgets")) {
		NEED(1);
		int cap = IS("fgets") && na > 1 ? (int)a[1].i : 512;
		int l = read_line_into(c, (uint64_t)a[0].i, cap);
		r->i = l < 0 ? 0 : a[0].i;
		if (l >= 0 && IS("fgets") && l + 2 <= cap) {
			c->mem[a[0].i + l] = '\n';
			c->mem[a[0].i + l + 1] = 0;
		}
		return 1;
	}
	if (IS("scanf")) {	/* %d %ld %lf %f %s %c, one line of input */
		NEED(1);
		char line[512];
		if (read_line(line, sizeof line) < 0) {
			r->i = -1;
			return 1;
		}
		const char *f = cstr(c, (uint64_t)a[0].i), *p = line;
		int ai = 1, got = 0;
		for (; *f && ai < na; f++) {
			if (*f != '%')
				continue;
			f++;
			int lng = 0;
			while (*f == 'l') {
				lng = 1;
				f++;
			}
			while (*p == ' ' || *p == '\t')
				p++;
			if (!*p)
				break;
			uint64_t dst = (uint64_t)a[ai++].i;
			if (*f == 'd' || *f == 'i') {
				const char *e;
				long v = strtol(p, &e, 10);
				if (e == p)
					break;
				store(c, lng ? &ty_long : &ty_int, dst, (struct val){ v, 0 });
				p = e;
			} else if (*f == 'f' || *f == 'g' || *f == 'e') {
				const char *e;
				double v = f_parse(p, &e);
				if (e == p)
					break;
				struct val dv = { 0, v };
				if (lng) {
					store(c, &ty_double, dst, dv);
				} else {
					store(c, &ty_double, dst, dv);	/* float is double here */
				}
				p = e;
			} else if (*f == 's') {
				int k = 0;
				while (p[k] && p[k] != ' ' && p[k] != '\t')
					k++;
				check_addr(c, dst, k + 1);
				memcpy(c->mem + dst, p, (size_t)k);
				c->mem[dst + (uint64_t)k] = 0;
				p += k;
			} else if (*f == 'c') {
				store(c, &ty_char, dst, (struct val){ (unsigned char)*p++, 0 });
			} else {
				break;
			}
			got++;
		}
		r->i = got;
		return 1;
	}
	if (IS("malloc")) {
		NEED(1);
		r->i = c_malloc(c, (uint64_t)a[0].i);
		return 1;
	}
	if (IS("calloc")) {
		NEED(2);
		r->i = c_malloc(c, (uint64_t)(a[0].i * a[1].i));
		return 1;
	}
	if (IS("realloc")) {
		NEED(2);
		uint32_t np = c_malloc(c, (uint64_t)a[1].i);
		if (np && a[0].i) {
			struct mhdr *h = (struct mhdr *)(c->mem + a[0].i - 16);
			memmove(c->mem + np, c->mem + a[0].i, MIN((size_t)(h->size & 0x7FFFFFFF), (size_t)a[1].i));
			c_free(c, (uint64_t)a[0].i);
		}
		r->i = np;
		return 1;
	}
	if (IS("free")) {
		NEED(1);
		c_free(c, (uint64_t)a[0].i);
		return 1;
	}
	if (IS("strlen")) {
		NEED(1);
		r->i = (long long)strlen(cstr(c, (uint64_t)a[0].i));
		return 1;
	}
	if (IS("strcpy") || IS("strcat") || IS("strncpy") || IS("strncat")) {
		NEED(2);
		const char *src = cstr(c, (uint64_t)a[1].i);
		uint64_t dst = (uint64_t)a[0].i;
		size_t sl = strlen(src);
		int cat = name[3] == 'c';
		int bounded = name[3] == 'n';
		if (bounded) {
			NEED(3);
			sl = MIN(sl, (size_t)a[2].i);
			cat = name[4] == 'c';
		}
		if (cat)
			dst += strlen(cstr(c, dst));
		check_addr(c, dst, (int)sl + 1);
		memmove(c->mem + dst, src, sl);
		c->mem[dst + sl] = 0;
		r->i = a[0].i;
		return 1;
	}
	if (IS("strcmp") || IS("strncmp") || IS("strcasecmp") || IS("stricmp")) {
		NEED(2);
		const char *x = cstr(c, (uint64_t)a[0].i), *y = cstr(c, (uint64_t)a[1].i);
		if (IS("strncmp")) {
			NEED(3);
			r->i = strncmp(x, y, (size_t)a[2].i);
		} else {
			r->i = IS("strcmp") ? strcmp(x, y) : strcasecmp(x, y);
		}
		return 1;
	}
	if (IS("strchr") || IS("strrchr") || IS("strstr")) {
		NEED(2);
		const char *s = cstr(c, (uint64_t)a[0].i);
		const char *hit = IS("strstr") ? strstr(s, cstr(c, (uint64_t)a[1].i)) :
		    IS("strchr") ? strchr(s, (int)a[1].i) : strrchr(s, (int)a[1].i);
		r->i = hit ? (long long)(hit - (const char *)c->mem) : 0;
		return 1;
	}
	if (IS("memset")) {
		NEED(3);
		check_addr(c, (uint64_t)a[0].i, (int)a[2].i);
		memset(c->mem + a[0].i, (int)a[1].i, (size_t)a[2].i);
		r->i = a[0].i;
		return 1;
	}
	if (IS("memcpy") || IS("memmove")) {
		NEED(3);
		check_addr(c, (uint64_t)a[0].i, (int)a[2].i);
		check_addr(c, (uint64_t)a[1].i, (int)a[2].i);
		memmove(c->mem + a[0].i, c->mem + a[1].i, (size_t)a[2].i);
		r->i = a[0].i;
		return 1;
	}
	if (IS("memcmp")) {
		NEED(3);
		check_addr(c, (uint64_t)a[0].i, (int)a[2].i);
		check_addr(c, (uint64_t)a[1].i, (int)a[2].i);
		r->i = memcmp(c->mem + a[0].i, c->mem + a[1].i, (size_t)a[2].i);
		return 1;
	}
	if (IS("atoi") || IS("atol")) {
		NEED(1);
		r->i = strtol(cstr(c, (uint64_t)a[0].i), NULL, 10);
		return 1;
	}
	if (IS("atof")) {
		NEED(1);
		r->d = f_parse(cstr(c, (uint64_t)a[0].i), NULL);
		return 1;
	}
	if (IS("abs") || IS("labs")) {
		NEED(1);
		r->i = a[0].i < 0 ? -a[0].i : a[0].i;
		return 1;
	}
	if (IS("rand")) {
		c->rng = c->rng * 1103515245u + 12345u;
		r->i = (c->rng >> 1) & 0x7FFFFFFF;
		return 1;
	}
	if (IS("srand")) {
		NEED(1);
		c->rng = (uint32_t)a[0].i;
		return 1;
	}
	if (IS("time")) {
		r->i = (long long)rtc_epoch();
		if (na && a[0].i)
			store(c, &ty_long, (uint64_t)a[0].i, *r);
		return 1;
	}
	if (IS("clock")) {
		r->i = (long long)uptime_ms();
		return 1;
	}
	if (IS("exit")) {
		c->exit_code = na ? (int)a[0].i : 0;
		c->exiting = 1;
		snprintf(c->err, sizeof c->err, "");
		arch_longjmp(&c->jb, 2);
	}
	if (IS("delay") || IS("sleep_ms") || IS("Sleep")) {
		NEED(1);
		sleep_ms((uint64_t)MAX(0, a[0].i));
		return 1;
	}
	if (IS("sleep")) {
		NEED(1);
		sleep_ms((uint64_t)MAX(0, a[0].i) * 1000);
		return 1;
	}
	if (IS("clrscr")) {
		con_clear();
		return 1;
	}
	if (IS("gotoxy")) {	/* Turbo C counts from 1 */
		NEED(2);
		con_gotoxy((int)a[0].i - 1, (int)a[1].i - 1);
		return 1;
	}
	if (IS("textcolor") || IS("textattr")) {
		NEED(1);
		con_attr((int)(a[0].i & 15), IS("textattr") ? (int)((a[0].i >> 4) & 15) : 0);
		return 1;
	}
	if (IS("system")) {
		NEED(1);
		r->i = shell_exec(cstr(c, (uint64_t)a[0].i));
		return 1;
	}
	if (IS("isdigit") || IS("isalpha") || IS("isalnum") || IS("isspace") || IS("isupper") ||
	    IS("islower") || IS("toupper") || IS("tolower") || IS("ispunct")) {
		NEED(1);
		int ch = (int)a[0].i;
		if (IS("isdigit")) r->i = isdigit(ch);
		else if (IS("isalpha")) r->i = isalpha(ch);
		else if (IS("isalnum")) r->i = isalnum(ch);
		else if (IS("isspace")) r->i = isspace(ch);
		else if (IS("isupper")) r->i = ch >= 'A' && ch <= 'Z';
		else if (IS("islower")) r->i = ch >= 'a' && ch <= 'z';
		else if (IS("ispunct")) r->i = ch > 32 && ch < 127 && !isalnum(ch);
		else if (IS("toupper")) r->i = toupper(ch);
		else r->i = tolower(ch);
		return 1;
	}
	/* math.h */
	struct { const char *n; double (*f)(double); } m1[] = {
		{ "sqrt", f_sqrt }, { "sin", f_sin }, { "cos", f_cos }, { "tan", f_tan },
		{ "atan", f_atan }, { "asin", f_asin }, { "acos", f_acos }, { "exp", f_exp },
		{ "log", f_log }, { "log10", f_log10 }, { "floor", f_floor }, { "ceil", f_ceil },
		{ "fabs", f_fabs },
	};
	for (size_t i = 0; i < NELEM(m1); i++)
		if (IS(m1[i].n)) {
			NEED(1);
			r->d = m1[i].f(ARGD(0));
			return 1;
		}
	if (IS("pow") || IS("atan2") || IS("fmod")) {
		NEED(2);
		r->d = IS("pow") ? f_pow(ARGD(0), ARGD(1)) : IS("atan2") ? f_atan2(ARGD(0), ARGD(1)) : f_fmod(ARGD(0), ARGD(1));
		return 1;
	}
	return 0;
	#undef IS
	#undef NEED
	#undef ARGD
}

/* Return types of the library, so the parser types expressions right. */
static void declare_builtins(struct cint *c)
{
	static const char *const dbl[] = {
		"sqrt", "sin", "cos", "tan", "atan", "asin", "acos", "exp", "log", "log10", "floor",
		"ceil", "fabs", "pow", "atan2", "fmod", "atof",
	};
	static const char *const ptr_char[] = {
		"strcpy", "strcat", "strncpy", "strncat", "strchr", "strrchr", "strstr", "gets", "fgets",
	};
	static const char *const ptr_void[] = { "malloc", "calloc", "realloc", "memset", "memcpy", "memmove" };
	struct type *pc = ptr_to(c, &ty_char), *pv = ptr_to(c, &ty_void);
	for (size_t i = 0; i < NELEM(dbl); i++) {
		struct func *f = calloc_c(c, sizeof *f);
		strncpy0(f->name, dbl[i], sizeof f->name);
		f->ret = &ty_double;
		f->next = c->funcs;
		c->funcs = f;
	}
	for (size_t i = 0; i < NELEM(ptr_char); i++) {
		struct func *f = calloc_c(c, sizeof *f);
		strncpy0(f->name, ptr_char[i], sizeof f->name);
		f->ret = pc;
		f->next = c->funcs;
		c->funcs = f;
	}
	for (size_t i = 0; i < NELEM(ptr_void); i++) {
		struct func *f = calloc_c(c, sizeof *f);
		strncpy0(f->name, ptr_void[i], sizeof f->name);
		f->ret = pv;
		f->next = c->funcs;
		c->funcs = f;
	}
	/* the usual names and types from the standard headers */
	static const struct { const char *name; long long val; } consts[] = {
		{ "NULL", 0 }, { "EOF", -1 }, { "RAND_MAX", 0x7FFFFFFF }, { "stdin", 0 },
		{ "stdout", 1 }, { "stderr", 2 }, { "EXIT_SUCCESS", 0 }, { "EXIT_FAILURE", 1 },
		{ "true", 1 }, { "false", 0 },
		{ "BLACK", 0 }, { "BLUE", 1 }, { "GREEN", 2 }, { "CYAN", 3 }, { "RED", 4 },
		{ "MAGENTA", 5 }, { "BROWN", 6 }, { "LIGHTGRAY", 7 }, { "DARKGRAY", 8 },
		{ "LIGHTBLUE", 9 }, { "LIGHTGREEN", 10 }, { "LIGHTCYAN", 11 }, { "LIGHTRED", 12 },
		{ "LIGHTMAGENTA", 13 }, { "YELLOW", 14 }, { "WHITE", 15 },
	};
	for (size_t i = 0; i < NELEM(consts); i++)
		add_sym(c, consts[i].name, SY_ENUM, &ty_int)->val = consts[i].val;
	add_sym(c, "size_t", SY_TYPEDEF, &ty_long);
	add_sym(c, "Window", SY_TYPEDEF, &ty_int);	/* desk.h */
	add_sym(c, "bool", SY_TYPEDEF, &ty_int);
	add_sym(c, "FILE", SY_TYPEDEF, &ty_int);
}

/* ---- running ------------------------------------------------------------------------------ */

static int cint_run_file_unlocked(const char *path, int argc, char **argv)
{
	struct cint *c = kzalloc(sizeof *c);
	if (!c) {
		kprintf("Not enough memory\n");
		return 1;
	}
	strncpy0(c->path, path, sizeof c->path);
	int err;
	size_t len;
	char *src = vfs_slurp(path, &len, &err);
	if (!src) {
		kprintf("%s - %s\n", vfs_strerror(err), path);
		kfree(c);
		return 1;
	}
	c->mem = kzalloc(MEMSZ);
	c->toks = kmalloc(sizeof(struct tok) * MAX_TOKENS);
	c->mtoks = kmalloc(sizeof(struct tok) * 4096);
	if (!c->mem || !c->toks || !c->mtoks) {
		kprintf("Not enough memory to run C programs\n");
		kfree(src);
		kfree(c->mem);
		kfree(c->toks);
		kfree(c->mtoks);
		kfree(c);
		return 1;
	}
	c->data_top = 16;
	c->heap_base = MEMSZ / 4;
	c->heap_top = c->heap_base;
	c->heap_end = MEMSZ - STACKSZ;
	c->sp = MEMSZ - 16;
	c->fp = c->sp;
	c->rng = (uint32_t)rtc_epoch();

	int rc = 0;
	int jr = arch_setjmp(&c->jb);
	if (jr == 0) {
		push_scope(c);
		declare_builtins(c);
		/* M_PI as a real constant: a tiny #define */
		lex(c, "#define M_PI 3.14159265358979323846\n");
		c->ntoks = 0;	/* keep the macro, drop the tokens */
		lex(c, src);
		parse_toplevel(c);
		struct func *mainf = find_func(c, "main");
		if (!mainf || !mainf->defined)
			cerr(c, 1, "no main() function");
		/* argv strings in program memory */
		uint32_t av = c_malloc(c, (uint64_t)(argc + 1) * 8);
		for (int i = 0; i < argc; i++) {
			const char *s = i == 0 ? path : argv[i];
			uint32_t sa = c_malloc(c, strlen(s) + 1);
			memcpy(c->mem + sa, s, strlen(s) + 1);
			uint64_t p = sa;
			memcpy(c->mem + av + (uint32_t)i * 8, &p, 8);
		}
		struct node callmain;
		memset(&callmain, 0, sizeof callmain);
		callmain.kind = N_CALL;
		callmain.fn = mainf;
		callmain.name = "main";
		struct node a0, a1;
		memset(&a0, 0, sizeof a0);
		memset(&a1, 0, sizeof a1);
		a0.kind = N_NUM;
		a0.ival = argc;
		a0.ty = &ty_int;
		a1.kind = N_NUM;
		a1.ival = av;
		a1.ty = ptr_to(c, ptr_to(c, &ty_char));
		if (mainf->nparams >= 2) {
			callmain.a = &a0;
			a0.next = &a1;
		}
		struct val r = call(c, &callmain);
		rc = (int)r.i;
	} else if (jr == 2 || c->exiting) {
		rc = c->exit_code;
	} else {
		if (c->col)
			con_putc('\n');
		con_attr(7, 0);
		kprintf("%s\n", c->err);
		rc = 1;
	}
	con_attr(7, 0);
	while (c->pool) {
		struct pnode *p = c->pool;
		c->pool = p->next;
		kfree(p);
	}
	kfree(src);
	kfree(c->mem);
	kfree(c->toks);
	kfree(c->mtoks);
	kfree(c);
	return rc;
}

/* The interpreter runs without the big kernel lock: on any core, in
 * parallel with everything else. Kernel calls go through gate.h. */

int cint_run_file(const char *path, int argc, char **argv)
{
	int held = bkl_release_all();
	int r = cint_run_file_unlocked(path, argc, argv);
	bkl_reacquire(held);
	return r;
}
