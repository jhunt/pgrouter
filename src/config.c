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

typedef struct __p PARSER;
typedef int (*parser_fn)(PARSER*);

struct __p {
	char *backend; /* current backend scope    */

	LEXER *l;      /* lexer to get tokens from */
	parser_fn f;   /* current parser function  */
};

static const char* token_name(int type)
{
	int i;
	for (i = 0; TOKEN_NAMES[i].name != NULL; i++) {
		if (TOKEN_NAMES[i].value == type) {
			return TOKEN_NAMES[i].name;
		}
	}
	return "(unknown)";
}

static void dump_token(TOKEN *t)
{
	char buf[256];
	int n = (t->length > 255 ? 255 : t->length);
	strncpy(buf, t->value, n);
	buf[n] = '\0';
	fprintf(stderr, "[token]: {%s(%d), '%s', %d}\n",
	                token_name(t->type), t->type, buf, t->length);
}

static void dump_lexer(LEXER *l)
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
		if (c == 0) {
			l->f = NULL;
			return token(T_EOS, NULL);
		}

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
		dump_lexer(l);
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

LEXER* lexer_init(const char *file, FILE *io)
{
	/* how much stuff is in *io ? */
	int off = fseek(io, 0, SEEK_END);
	if (off < 0) {
		return NULL;
	}
	long size = ftell(io);
	if (size < 0) {
		return NULL;
	}
	if (fseek(io, off, SEEK_SET) < 0) {
		return NULL;
	}

	/* is it too much stuff? */
	if (size > INT_MAX) {
		errno = EFBIG;
		return NULL;
	}


	/* set up the LEXER state */
	LEXER *l = malloc(sizeof(LEXER));
	if (!l) {
		return NULL;
	}
	l->file = strdup(file);
	l->line = l->col   = 0;
	l->pos  = l->start = 0;
	l->f    = lex_any;
	l->max  = (int)size;
	l->src  = calloc(l->max, sizeof(char));
	if (!l->src) {
		free(l);
		return NULL;
	}

	/* read in from the file */
	size_t n;
	int pos = 0, left = l->max;
	while ((n = fread(l->src + pos, left, sizeof(char), io)) > 0) {
		left -= n;
		pos  += n;
	}

	return l;
}

static char* as_string(TOKEN *t)
{
	char *s;
	const char *p;
	int left, len;

	switch (t->type) {
	case T_TYPE_BAREWORD:
	case T_TYPE_ADDRESS:
		s = calloc(t->length + 1, sizeof(char));
		if (!s) {
			return NULL;
		}
		strncpy(s, t->value, t->length);
		return s;

	case T_TYPE_QSTRING:
		len = 0;
		left = t->length;

		for (p = t->value; left > 0; left--, p++) {
			if (*p == '\\') {
				left--;
				p++;
			}
			len++;
		}
		s = calloc(len + 1, sizeof(char));
		if (!s) {
			return NULL;
		}

		len = 0;
		left = t->length;
		for (p = t->value; left > 0; left--, p++) {
			if (*p == '\\') {
				left--;
				p++;
				switch (*p) {
				case 't':  s[len++] = '\t';
				case 'r':  s[len++] = '\r';
				case 'n':  s[len++] = '\n';
				case '\\':
				case '\'':
				case '"':
					s[len++] = *p;
				defaut:
					// FIXME: complain!
					s[len++] = *p;
				}
				continue;
			}

			s[len++] = *p;
		}
		return s;

	default:
		return NULL;
	}
}

static int as_int(TOKEN *t)
{
	switch (t->type) {
	case T_TYPE_INTEGER:
	case T_TYPE_TIME:
	case T_TYPE_SIZE:
		return t->semval.i;

	default:
		return -77;
	}
}

static int parse(PARSER *p)
{
	while (p->f && p->f(p) == 0)
		;
	return 0;
}

static int parse_backend(PARSER *p);
static int parse_health(PARSER *p);
static int parse_tls(PARSER *p);

static int parse_top(PARSER *p)
{
	TOKEN t1, t2;
	char *s;
	int i;

	t1 = emit(p->l);
	switch (t1.type) {
	case T_KEYWORD_LISTEN:
	case T_KEYWORD_MONITOR:
	case T_KEYWORD_HBA:
	case T_KEYWORD_USER:
	case T_KEYWORD_GROUP:
	case T_KEYWORD_PIDFILE:
		t2 = emit(p->l);
		s = as_string(&t2);
		if (!s) {
			return -1;
		}
		switch (t1.type) {
		case T_KEYWORD_LISTEN:   printf("listen on '%s'\n", s);      break;
		case T_KEYWORD_MONITOR:  printf("monitor on '%s'\n", s);     break;
		case T_KEYWORD_HBA:      printf("hba file is at '%s'\n", s); break;
		case T_KEYWORD_USER:     printf("run as user '%s'\n", s);    break;
		case T_KEYWORD_GROUP:    printf("run as group '%s'\n", s);   break;
		case T_KEYWORD_PIDFILE:  printf("pid file is at '%s'\n", s); break;
		}
		return 0;

	case T_KEYWORD_LOG:
		t2 = emit(p->l);
		switch (t2.type) {
		case T_KEYWORD_ERROR: printf("log level is ERROR\n"); break;
		case T_KEYWORD_INFO:  printf("log level is INFO\n");  break;
		case T_KEYWORD_DEBUG: printf("log level is DEBUG\n"); break;
		default:
			printf("bad log level\n");
			return 1;
		}
		return 0;

	case T_KEYWORD_WORKERS:
		t2 = emit(p->l);
		dump_token(&t2);
		i = as_int(&t2);
		if (i < 1) {
			printf("invalid number of workers (%d)!\n", i);
			return -1;
		}
		printf("spin up %d workers\n", i);
		return 0;

	case T_KEYWORD_TLS:
		t2 = emit(p->l);
		if (t2.type != T_OPEN) {
			printf("bad follow-on to tls\n");
			return -1;
		}
		p->f = parse_tls;
		return 0;

	case T_KEYWORD_HEALTH:
		t2 = emit(p->l);
		if (t2.type != T_OPEN) {
			printf("bad follow-on to health\n");
			return -1;
		}
		p->f = parse_health;
		return 0;

	case T_KEYWORD_BACKEND:
		t2 = emit(p->l);
		if (t2.type == T_KEYWORD_DEFAULT) {
			free(p->backend);
			p->backend = NULL;

		} else {
			s = as_string(&t2);
			if (!s) {
				printf("bad backend scope!\n");
				return -1;
			}

			free(p->backend);
			p->backend = s;
		}
		printf("=== backend %s ====\n", p->backend ? p->backend : "defaults");

		t2 = emit(p->l);
		if (t2.type != T_OPEN) {
			printf("bad follow-on to backend\n");
			return -1;
		}
		p->f = parse_backend;
		return 0;

	case T_EOS:
		p->f = NULL;
		return 0;

	default:
		dump_token(&t1);
		fprintf(stderr, "bad toplevel\n");
		return 1;
	}
}

static int parse_backend(PARSER *p)
{
	TOKEN t1, t2;
	char *s;
	int i;

	t1 = emit(p->l);
	switch (t1.type) {
	case T_KEYWORD_TLS:
		t2 = emit(p->l);
		switch (t2.type) {
		case T_KEYWORD_ON:
			printf("use TLS for %s backend\n",
			       p->backend ? p->backend : "default");
			break;

		case T_KEYWORD_OFF:
			printf("no TLS for %s backend\n",
			       p->backend ? p->backend : "default");
			break;

		case T_KEYWORD_SKIPVERIFY:
			printf("use (unverified) TLS for %s backend\n",
			       p->backend ? p->backend : "default");
			break;

		default:
			printf("bad! bad bad bad!\n");
			return 1;
		}
		return 0;

	case T_KEYWORD_LAG:
	case T_KEYWORD_WEIGHT:
		t2 = emit(p->l);
		i = as_int(&t2);
		if (i < 1) {
			printf("invalid number (%d)!\n", i);
			return -1;
		}
		switch (t1.type) {
		case T_KEYWORD_LAG:    printf("lag %d\n", i); break;
		case T_KEYWORD_WEIGHT: printf("weight %d\n", i);  break;
		}
		return 0;

	case T_CLOSE:
		p->f = parse_top;
		return 0;

	case T_TERMX:
		return 0;

	default:
		printf("unexpected token in backend stanza\n");
		return 1;
	}
	return 0;
}

static int parse_health(PARSER *p)
{
	TOKEN t1, t2;
	char *s;
	int i;

	t1 = emit(p->l);
	switch (t1.type) {
	case T_KEYWORD_DATABASE:
	case T_KEYWORD_USERNAME:
	case T_KEYWORD_PASSWORD:
		t2 = emit(p->l);
		s = as_string(&t2);
		if (!s) {
			return -1;
		}
		switch (t1.type) {
		case T_KEYWORD_DATABASE: printf("health check via db '%s'\n", s);  break;
		case T_KEYWORD_USERNAME: printf("health check as user '%s'\n", s); break;
		case T_KEYWORD_PASSWORD: printf("health check using '%s'\n", s);   break;
		}
		return 0;

	case T_KEYWORD_TIMEOUT:
	case T_KEYWORD_CHECK:
		t2 = emit(p->l);
		i = as_int(&t2);
		if (i < 1) {
			printf("invalid number (%d)!\n", i);
			return -1;
		}
		switch (t1.type) {
		case T_KEYWORD_TIMEOUT: printf("timeout %ds\n", i); break;
		case T_KEYWORD_CHECK:   printf("check @%ds\n", i);  break;
		}
		return 0;

	case T_CLOSE:
		p->f = parse_top;
		return 0;

	default:
		printf("unexpected token in health stanza\n");
		return 1;
	}
}

static int parse_tls(PARSER *p)
{
	TOKEN t1, t2;
	char *s;

	t1 = emit(p->l);
	switch (t1.type) {
	case T_KEYWORD_CIPHERS:
	case T_KEYWORD_CERT:
	case T_KEYWORD_KEY:
		t2 = emit(p->l);
		s = as_string(&t2);
		if (!s) {
			return -1;
		}
		switch (t1.type) {
		case T_KEYWORD_CIPHERS: printf("use ciphers '%s'\n", s);     break;
		case T_KEYWORD_CERT:    printf("cert file is at '%s'\n", s); break;
		case T_KEYWORD_KEY:     printf("key file is at '%s'\n", s);  break;
		}
		break;

	case T_CLOSE:
		p->f = parse_top;
		return 0;

	default:
		printf("unexpected token in tls stanza\n");
		return 1;
	}

	return 0;
}

PARSER* parser_init(const char *file, FILE *io, int reload)
{
	PARSER *p = malloc(sizeof(PARSER));
	if (!p) {
		return NULL;
	}

	p->backend = NULL;
	p->f = parse_top;
	p->l = lexer_init(file, io);
	if (!p->l) {
		free(p);
		return NULL;
	}

	return p;
}

int pgr_configure(CONTEXT *c, FILE *io, int reload)
{
	PARSER *p = parser_init("<io>", io, reload);
	return parse(p);
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
