#include "pgrouter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#define C_ALPHA   "abcdefghijklmnopqrstuvwxyz" "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define C_NUMERIC "0123456789"
#define C_ALPHANUMERIC C_ALPHA C_NUMERIC

#include "config.inc"

typedef struct {
	int type;           /* type of token (a T_* constant)  */

	const char *value;  /* lexical value (not NULL-term'd) */
	int length;         /* length of value                 */

	union {
		unsigned int i; /* converted semantic integer val  */
		double       f; /* converted semantic float val    */
	} semval;
} TOKEN;

typedef struct __l LEXER;
typedef TOKEN (*lexer_fn)(LEXER*);

struct __l {
	char *file; /* path to file being parsed */
	char *src;  /* source, as read from file */
	int line;   /* current line number       */
	int col;    /* current column number     */

	int pos;    /* abs. offset into src      */
	int max;    /* size of src in bytes      */
	int start;  /* start of current token    */

	lexer_fn f; /* current lexer function    */
};

static void dump(LEXER *l)
{
	char buf[256];
	int n = (l->pos - l->start > 255 ? 255 : l->pos - l->start);
	strncpy(buf, l->src + l->start, n);
	buf[n] = '\0';
	fprintf(stderr, "[lexer]: at %s:%d:%d, %d/%d s=%d '%s'\n",
	                l->file, l->line, l->col,
	                l->pos, l->max, l->start,
	                buf);
}

static char next(LEXER *l)
{
	if (l->pos >= l->max) {
		return 0;
	}
	l->col++;
	return l->src[l->pos++];
}

static void restart(LEXER *l)
{
	l->pos = l->start;
}

static void ignore(LEXER *l)
{
	l->start = l->pos;
}

static void backup(LEXER *l)
{
	l->col--;
	l->pos--;
}

static char peek(LEXER *l)
{
	char c = next(l);
	backup(l);
	return c;
}

static int accept_one(LEXER *l, const char *valid)
{
	if (strchr(valid, next(l)) != NULL) {
		return 1;
	}
	backup(l);
	return 0;
}

static int accept_all(LEXER *l, const char *valid)
{
	int n = 0;
	while (accept_one(l, valid))
		n++;
	return n;
}

static TOKEN token(int type, LEXER *l) {
	TOKEN t = { type, NULL, 0 };
	if (l) {
		dump(l);
		t.value  = l->src + l->start;
		t.length = l->pos - l->start;
		ignore(l);
	}
	return t;
}

#define TRY_AGAIN token(T_RESTART, NULL)

static TOKEN emit(LEXER *l)
{
	TOKEN t;
	while (l->f) {
		t = l->f(l);
		if (t.type == T_RESTART) {
			continue;
		}
		return t;
	}
}

static TOKEN lex_bareword(LEXER *l);
static TOKEN lex_comment(LEXER *l);
static TOKEN lex_numeric(LEXER *l);
static TOKEN lex_qstring(LEXER *l);
static TOKEN lex_wildcard(LEXER *l);

static TOKEN lex_any(LEXER *l)
{
	for (;;) {
		char c = next(l);

		if (c == '\n') {
			ignore(l);
			l->line++;
			l->col = 0;
			continue;
		}

		if (isspace(c)) {
			ignore(l);
			continue;
		}

		if (c == '{') {
			ignore(l);
			return token(T_OPEN, NULL);
		}

		if (c == '}') {
			ignore(l);
			return token(T_CLOSE, NULL);
		}

		if (c == ';') {
			ignore(l);
			return token(T_TERMX, NULL);
		}

		if (c == '#') {
			l->f = lex_comment;
			ignore(l);
			return TRY_AGAIN;
		}

		if (c == '\'' || c == '"') {
			l->f = lex_qstring;
			return TRY_AGAIN;
		}

		if (isdigit(c)) {
			l->f = lex_numeric;
			return TRY_AGAIN;
		}

		if (isalnum(c) || c == '/') {
			l->f = lex_bareword;
			return TRY_AGAIN;
		}

		if (c == '*') {
			l->f = lex_wildcard;
			return TRY_AGAIN;
		}

		l->f = NULL;
		return token(T_ERROR, NULL);
	}

	l->f = NULL;
	return token(T_EOS, NULL);
}

static TOKEN lex_bareword(LEXER *l)
{
	l->f = lex_any;

	accept_all(l, C_ALPHANUMERIC "_-+/:.,!");

	char *value = l->src + l->start;
	int length  = l->pos - l->start;

	/* is this bareword actually a keyword? */
	int i;
	for (i = 0; KEYWORDS[i].value >= 0 && KEYWORDS[i].match != NULL; i++) {
		if (strncasecmp(value, KEYWORDS[i].match, length) == 0) {
			ignore(l);
			return token(KEYWORDS[i].value, NULL);
		}
	}

	/* nope, return it as a bareword */
	return token(T_TYPE_BAREWORD, l);
}

static TOKEN lex_comment(LEXER *l)
{
	while (next(l) != '\n')
		;
	l->line++;
	ignore(l);

	l->f = lex_any;
	return TRY_AGAIN;
}

static TOKEN lex_numeric(LEXER *l)
{
	/*
	   supported numeric formats:

	   \d+.\d+.\d+.\d+:\d+  is an ip:port (a BAREWORD)
	   \d+.\d+.\d+.\d+      is an ip (another BAREWORD)
	   \d+[kKmMgG]?b        is a size
	   \d+[smh]             is a time
	   \d+.\d+              is a decimal
	   \d+                  is an integer
	 */

	TOKEN t;
	char c;
	unsigned int ival = 0;

	l->f = lex_any;

	/* first let's see if this is in fact an IP address */
	if (accept_all(l, C_NUMERIC) && accept_one(l, ".")
	 && accept_all(l, C_NUMERIC) && accept_one(l, ".")
	 && accept_all(l, C_NUMERIC) && accept_one(l, ".")
	 && accept_all(l, C_NUMERIC)) {
		if (accept_one(l, ":") && !accept_all(l, C_NUMERIC)) {
			return token(T_ERROR, NULL);
		}
		return token(T_TYPE_ADDRESS, l);
	}
	restart(l);

	for (;;) {
		c = next(l);
		if (strchr(C_NUMERIC, c) == NULL) {
			backup(l);
			break;
		}
		ival = ival * 10 + (c - '0');
	}
	c = next(l);

	if (c == '.') {
		double factor = 10;
		double fval = ival;
		for (;;) {
			c = next(l);
			if (strchr(C_NUMERIC, c) == NULL) {
				backup(l);
				break;
			}
			fval = fval + ((c - '0') / factor);
			factor *= 10;
		}
		t = token(T_TYPE_DECIMAL, l);
		t.semval.f = fval;
		return t;
	}

	if (strchr("kKmMgGbB", c) != NULL) {
		int factor = 1;
		switch (c) {
		case 'g': case 'G': factor *= 1024;
		case 'm': case 'M': factor *= 1024;
		case 'k': case 'K': factor *= 1024;
		}
		if (c != 'b' && c != 'B') {
			c = next(l);
		}
		if (c == 'b' || c == 'B') {
			t = token(T_TYPE_SIZE, l);
			t.semval.i = ival * factor;
			return t;
		}
		restart(l);
	}

	if (strchr("sSmMhH", c) != NULL) {
		int factor = 1;
		switch (c) {
		case 'h': case 'H': factor *= 60;
		case 'm': case 'M': factor *= 60;
		case 's': case 'S': factor *= 60;
		}
		t = token(T_TYPE_TIME, l);
		t.semval.i = ival * factor;
		return t;
	}

	backup(l);
	t = token(T_TYPE_INTEGER, l);
	t.semval.i = ival;
	return t;
}

static TOKEN lex_qstring(LEXER *l)
{
	TOKEN t;
	char c, q;

	l->f = lex_any;

	backup(l);
	q = next(l);
	for (;;) {
		c = next(l);
		if (c == q) {
			break;
		}
		if (c == '\\') {
			c = next(l);
		}
	}

	return token(T_TYPE_QSTRING, l);
}

static TOKEN lex_wildcard(LEXER *l)
{
	l->f = lex_any;

	if (accept_one(l, ":") && accept_all(l, C_NUMERIC)) {
		return token(T_TYPE_ADDRESS, l);
	}
	return token(T_ERROR, NULL);
}

int pgr_configure(CONTEXT *c, FILE *io, int reload)
{
	LEXER l = {
		.file  = strdup("<io>"),
		.line  = 1,
		.col   = 0,
		.pos   = 0,
		.start = 0,
		.f     = lex_any,
	};

	int off = fseek(io, 0, SEEK_END);
	if (off < 0) {
		return -1;
	}

	long size = ftell(io);
	if (size < 0) {
		return -1;
	}

	if (size > INT_MAX) {
		/* FIXME: need an E* code here for errno... */
		return -1;
	}

	l.max = (int)size;
	if (fseek(io, off, SEEK_SET) < 0) {
		return -1;
	}

	l.src = calloc(l.max, sizeof(char));
	if (!l.src) {
		return -1;
	}

	size_t n;
	int pos = 0, left = l.max;
	while ((n = fread(l.src + pos, left, sizeof(char), io)) > 0) {
		left -= n;
		pos  += n;
	}

	/* FIXME: this is terribly wrong */
	TOKEN t;
	char buf[256];
	int deadline = 80;
	do {
		dump(&l);
		t = emit(&l);
		if (t.value != NULL) {
			size_t n = (t.length > 255 ? 255 : t.length);
			memcpy(buf, t.value, n);
			buf[n] = '\0';
		} else {
			buf[0] = '\0';
		}
		fprintf(stderr, "got a [%d] token (%s)\n", t.type, buf);
	} while (t.type != T_EOS && t.type != T_ERROR && deadline-- > 0);
	return 0;
}

#ifdef PTEST
int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "USAGE: %s /path/to/config/file\n", argv[0]);
		return 1;
	}

	FILE *io = fopen(argv[1], "r");
	if (!io) {
		fprintf(stderr, "%s: [%d] %s\n", argv[1], errno, strerror(errno));
		return 2;
	}

	if (pgr_configure(NULL, io, 0) != 0) {
		fprintf(stderr, "pgr_configure: [%d] %s\n", errno, strerror(errno));
		return 3;
	}

	return 0;
}
#endif
