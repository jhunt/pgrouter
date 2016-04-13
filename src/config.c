#include "pgrouter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <syslog.h>

#define C_ALPHA   "abcdefghijklmnopqrstuvwxyz" "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define C_NUMERIC "0123456789"
#define C_ALPHANUMERIC C_ALPHA C_NUMERIC

#include "config.gen.c"

typedef struct {
	int value;
	int set;
} intval_t;

typedef struct {
	char *value;
	int   set;
} strval_t;


struct _backend {
	char *id;
	char *hostname;
	int port;

	intval_t tls;
	intval_t weight;
	intval_t lag;

	struct _backend *next;
};

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
	struct _backend *backends;
	struct _backend *current;

	intval_t workers;
	intval_t loglevel;

	intval_t health_interval;
	intval_t health_timeout;
	strval_t health_database;
	strval_t health_username;
	strval_t health_password;

	strval_t listen;
	strval_t monitor;
	strval_t hbafile;
	strval_t pidfile;
	strval_t tls_ciphers;
	strval_t tls_certfile;
	strval_t tls_keyfile;
	strval_t user;
	strval_t group;

	LEXER *l;      /* lexer to get tokens from */
	parser_fn f;   /* current parser function  */
};

static void set_str(strval_t *x, const char *v)
{
	free(x->value);
	x->set = 1;
	x->value = strdup(v);
}

static char* get_str(const char *a, strval_t *b, strval_t *c)
{
	if (c && c->set) {
		return c->value;
	}
	if (b && b->set) {
		return b->value;
	}
	return strdup(a);
}

static void set_int(intval_t *x, int v)
{
	x->set = 1;
	x->value = v;
}

static int get_int(int a, intval_t *b, intval_t *c)
{
	if (c && c->set) {
		return c->value;
	}
	if (b && b->set) {
		return b->value;
	}
	return a;
}

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

static int bound(int x, int lo, int hi)
{
	return (x < lo ? lo : x > hi ? hi : x);
}

static void dump_token(TOKEN *t)
{
	char buf[256];
	int i, j;
	for (i = 0, j = 0; i < 255 && j < t->length; j++) {
		switch (t->value[j]) {
		case '\n':
		case '\r':
		case '\t':
			if (i > 253) {
				goto done;
			}
			buf[i++] = '\\';
			switch(t->value[j]) {
			case '\n': buf[i++] = 'n'; break;
			case '\r': buf[i++] = 'r'; break;
			case '\t': buf[i++] = 't'; break;
			}
			break;

		default:
			buf[i++] = t->value[j];
		}
	}
done:
	buf[i] = '\0';
	pgr_logf(stderr, LOG_DEBUG, "got a token {%s(%d), '%s', %d}",
			token_name(t->type), t->type, buf, t->length);
}

static void dump_lexer(LEXER *l)
{
	char buf[256];
	int n = bound(l->pos - l->start, 0, 255);
	strncpy(buf, l->src + l->start, n);
	buf[n] = '\0';
	pgr_logf(stderr, LOG_DEBUG, "lexer at %s:%d:%d, %d/%d s=%d '%s'",
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
	char c = next(l);
	if (c != 0 && strchr(valid, c) != NULL) {
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
		dump_token(&t);
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
	int n = 0;
	char c;
	for (c = next(l); c != '\0' && c != '\n'; c = next(l))
		n++;
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
		if (c == 0) {
			break;
		}
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
			if (c == 0) {
				break;
			}
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
		if (c == 0) {
			return token(T_ERROR, NULL);
		}
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
	pgr_logf(stderr, LOG_DEBUG, "intializing a new lexer for %s (io %p)", file, io);

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
	pgr_logf(stderr, LOG_DEBUG, "looks like there are %lu bytes of data to parse", size);

	/* is it too much stuff? */
	if (size > INT_MAX) {
		pgr_logf(stderr, LOG_ERR, "%s contains %lu bytes of data; "
				"which is higher than INT_MAX (%u); aborting",
				size, INT_MAX);
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
	l->src  = calloc(l->max + 1, sizeof(char));
	if (!l->src) {
		free(l);
		return NULL;
	}

	pgr_logf(stderr, LOG_INFO, "lexer state initialized; reading from io %p", io);
	size_t n;
	int pos = 0, left = l->max;
	while ((n = fread(l->src + pos, sizeof(char), left, io)) > 0) {
		left -= n;
		pos  += n;
		pgr_logf(stderr, LOG_DEBUG, "read %u bytes, current position %d/%d, %d bytes left",
				n, pos, size, left);
	}
	pgr_logf(stderr, LOG_DEBUG, "final position %d/%d, %d bytes left",
			pos, size, left);
	pgr_logf(stderr, LOG_DEBUG, "src:\n```\n%s```", l->src);
	pgr_logf(stderr, LOG_INFO, "set up to lex %s, "
			"starting at %d:%d (position %d/%d, token at %d)",
			l->file, l->line, l->col, l->pos, l->max, l->start);
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
		left = t->length - 2; /* strip quotes */

		for (p = t->value + 1; left > 0; left--, p++) {
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
		left = t->length - 2; /* strip quotes */
		for (p = t->value + 1; left > 0; left--, p++) {
			if (*p == '\\') {
				left--;
				p++;
				switch (*p) {
				case 't':  s[len++] = '\t'; break;
				case 'r':  s[len++] = '\r'; break;
				case 'n':  s[len++] = '\n'; break;
				case '\\': s[len++] = '\\'; break;
				case '\'': s[len++] = '\''; break;
				case '"':  s[len++] = '"';  break;
				default:
					pgr_logf(stderr, LOG_INFO, "handling '\\%c' as just '%c', "
							"but you shouldn't rely on that behavior", *p, *p);
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

static struct _backend* make_backend(const char *id)
{
	struct _backend *b = calloc(1, sizeof(struct _backend));
	if (!b) {
		fprintf(stderr, "failed to allocate new backend: %s\n", strerror(errno));
		return NULL;
	}
	if (id) {
		b->id = strdup(id);
		/* FIXME: parse id */
	} else {
		set_int(&b->tls,    BACKEND_TLS_OFF);
		set_int(&b->weight, 1);
		set_int(&b->lag,    100);
	}
	return b;
}

static struct _backend* backend(PARSER *p, const char *id)
{
	/* default backend */
	if (id == NULL) {
		return p->backends;
	}

	struct _backend *b;
	for (b = p->backends; b->next != NULL; b = b->next) {
		if (strcmp(b->next->id, id) == 0) {
			return b->next;
		}
	}
	b->next = make_backend(id);
	return b->next;
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
		case T_KEYWORD_LISTEN:   set_str(&p->listen, s);  break;
		case T_KEYWORD_MONITOR:  set_str(&p->monitor, s); break;
		case T_KEYWORD_HBA:      set_str(&p->hbafile, s); break;
		case T_KEYWORD_USER:     set_str(&p->user, s);    break;
		case T_KEYWORD_GROUP:    set_str(&p->group, s);   break;
		case T_KEYWORD_PIDFILE:  set_str(&p->pidfile, s); break;
		}
		return 0;

	case T_KEYWORD_LOG:
		t2 = emit(p->l);
		switch (t2.type) {
		case T_KEYWORD_ERROR: set_int(&p->loglevel, LOG_ERR);   break;
		case T_KEYWORD_INFO:  set_int(&p->loglevel, LOG_INFO);  break;
		case T_KEYWORD_DEBUG: set_int(&p->loglevel, LOG_DEBUG); break;
		default:
			printf("bad log level\n");
			return 1;
		}
		return 0;

	case T_KEYWORD_WORKERS:
		t2 = emit(p->l);
		switch (t2.type) {
		case T_TYPE_INTEGER:
		case T_TYPE_TIME:
		case T_TYPE_SIZE:
			i = t2.semval.i;
			break;

		default:
			dump_token(&t2);
			printf("unexpected token\n");
			return 1;
		}
		if (i < 1) {
			printf("invalid number of workers (%d)!\n", i);
			return 1;
		}
		set_int(&p->workers, i);
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
			p->current = backend(p, NULL);

		} else {
			s = as_string(&t2);
			if (!s) {
				printf("bad backend scope!\n");
				return -1;
			}
			p->current = backend(p, s);
			free(s);
		}

		if (!p->current) {
			return -1;
		}

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
		fprintf(stderr, "bad toplevel\n");
		p->f = NULL;
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
		case T_KEYWORD_ON:         set_int(&p->current->tls, BACKEND_TLS_VERIFY);   break;
		case T_KEYWORD_OFF:        set_int(&p->current->tls, BACKEND_TLS_OFF);      break;
		case T_KEYWORD_SKIPVERIFY: set_int(&p->current->tls, BACKEND_TLS_NOVERIFY); break;

		default:
			fprintf(stderr, "unexpected token!\n");
			return 1;
		}
		return 0;

	case T_KEYWORD_LAG:
		t2 = emit(p->l);
		switch (t2.type) {
		case T_TYPE_INTEGER:
		case T_TYPE_SIZE:
			i = t2.semval.i;
			break;

		default:
			fprintf(stderr, "unexpected token!\n");
			return 1;
		}

		if (i < 0) {
			fprintf(stderr, "invalid lag value: %d\n", i);
			return 1;
		}
		set_int(&p->current->lag, i);
		return 0;

	case T_KEYWORD_WEIGHT:
		t2 = emit(p->l);
		switch (t2.type) {
		case T_TYPE_INTEGER: i = t2.semval.i;              break;
		case T_TYPE_DECIMAL: i = (int)(t2.semval.f * 100); break;

		default:
			fprintf(stderr, "unexpected token!\n");
			return 1;
		}

		if (i < 0) {
			printf("invalid backend weight factor: %d\n", i);
			return 1;
		}
		set_int(&p->current->weight, i);
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
		case T_KEYWORD_DATABASE: set_str(&p->health_database, s); break;
		case T_KEYWORD_USERNAME: set_str(&p->health_username, s); break;
		case T_KEYWORD_PASSWORD: set_str(&p->health_password, s); break;
		}
		return 0;

	case T_KEYWORD_TIMEOUT:
	case T_KEYWORD_CHECK:
		t2 = emit(p->l);
		switch (t2.type) {
		case T_TYPE_INTEGER:
		case T_TYPE_TIME:
			i = t2.semval.i;
			break;

		default:
			fprintf(stderr, "unexpected token!\n");
			return 1;
		}

		if (i < 0) {
			fprintf(stderr, "invalid value: %d\n", i);
			return 1;
		}
		switch (t1.type) {
		case T_KEYWORD_TIMEOUT: set_int(&p->health_timeout, i);  break;
		case T_KEYWORD_CHECK:   set_int(&p->health_interval, i); break;
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
		case T_KEYWORD_CIPHERS: set_str(&p->tls_ciphers, s);  break;
		case T_KEYWORD_CERT:    set_str(&p->tls_certfile, s); break;
		case T_KEYWORD_KEY:     set_str(&p->tls_keyfile, s);  break;
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
	PARSER *p = calloc(1, sizeof(PARSER));
	if (!p) {
		return NULL;
	}

	p->backends = make_backend(NULL);
	p->f = parse_top;
	p->l = lexer_init(file, io);
	if (!p->l) {
		free(p);
		return NULL;
	}

	return p;
}

int pgr_configure(CONTEXT *c, const char *file, int reload)
{
	int rc;
	FILE *io = stdin;
	if (strcmp(file, "-") != 0) {
		io = fopen(file, "r");
		if (!io) {
			return 1;
		}
	}
	PARSER *p = parser_init(file, io, reload);

	rc = parse(p);
	if (rc != 0) {
		return rc;
	}

	/* update what can be updated */
	if (p->workers.set) {
		c->workers = p->workers.value;
	}
	if (p->loglevel.set) {
		c->loglevel = p->loglevel.value;
	}

	if (p->health_interval.set) {
		c->health.interval = p->health_interval.value;
	}
	if (p->health_timeout.set) {
		c->health.timeout = p->health_timeout.value;
	}
	if (p->health_database.set) {
		free(c->health.database);
		c->health.database = p->health_database.value;
	}
	if (p->health_username.set) {
		free(c->health.username);
		c->health.username = p->health_username.value;
	}
	if (p->health_password.set) {
		free(c->health.password);
		c->health.password = p->health_password.value;
	}

	if (p->listen.set) {
		if (!reload) {
			c->startup.frontend = p->listen.value;
		} else if (strcmp(p->listen.value, c->startup.frontend) != 0) {
			fprintf(stderr, "ignoring new value for `listen %s`; retaining old value '%s'\n",
			                p->listen.value, c->startup.frontend);
			free(p->listen.value);
			p->listen.value = c->startup.frontend;
		}
	}
	if (p->monitor.set) {
		if (!reload) {
			c->startup.monitor = p->monitor.value;
		} else if (strcmp(p->monitor.value, c->startup.monitor) != 0) {
			fprintf(stderr, "ignoring new value for `monitor %s`; retaining old value '%s'\n",
			                p->monitor.value, c->startup.monitor);
			free(p->monitor.value);
			p->monitor.value = c->startup.monitor;
		}
	}
	if (p->hbafile.set) {
		if (!reload) {
			c->startup.hbafile = p->hbafile.value;
		} else if (strcmp(p->hbafile.value, c->startup.hbafile) != 0) {
			fprintf(stderr, "ignoring new value for `hba %s`; retaining old value '%s'\n",
			                p->hbafile.value, c->startup.hbafile);
			free(p->hbafile.value);
			p->hbafile.value = c->startup.hbafile;
		}
	}
	if (p->pidfile.set) {
		if (!reload) {
			c->startup.pidfile = p->pidfile.value;
		} else if (strcmp(p->pidfile.value, c->startup.pidfile) != 0) {
			fprintf(stderr, "ignoring new value for `pidfile %s`; retaining old value '%s'\n",
			                p->pidfile.value, c->startup.pidfile);
			free(p->pidfile.value);
			p->pidfile.value = c->startup.pidfile;
		}
	}
	if (p->tls_ciphers.set) {
		if (!reload) {
			c->startup.tls_ciphers = p->tls_ciphers.value;
		} else if (strcmp(p->tls_ciphers.value, c->startup.tls_ciphers) != 0) {
			fprintf(stderr, "ignoring new value for `tls_ciphers %s`; retaining old value '%s'\n",
			                p->tls_ciphers.value, c->startup.tls_ciphers);
			free(p->tls_ciphers.value);
			p->tls_ciphers.value = c->startup.tls_ciphers;
		}
	}
	if (p->tls_certfile.set) {
		if (!reload) {
			c->startup.tls_certfile = p->tls_certfile.value;
		} else if (strcmp(p->tls_certfile.value, c->startup.tls_certfile) != 0) {
			fprintf(stderr, "ignoring new value for `tls_certfile %s`; retaining old value '%s'\n",
			                p->tls_certfile.value, c->startup.tls_certfile);
			free(p->tls_certfile.value);
			p->tls_certfile.value = c->startup.tls_certfile;
		}
	}
	if (p->tls_keyfile.set) {
		if (!reload) {
			c->startup.tls_keyfile = p->tls_keyfile.value;
		} else if (strcmp(p->tls_keyfile.value, c->startup.tls_keyfile) != 0) {
			fprintf(stderr, "ignoring new value for `tls_keyfile %s`; retaining old value '%s'\n",
			                p->tls_keyfile.value, c->startup.tls_keyfile);
			free(p->tls_keyfile.value);
			p->tls_keyfile.value = c->startup.tls_keyfile;
		}
	}
	if (p->user.set) {
		if (!reload) {
			c->startup.user = p->user.value;
		} else if (strcmp(p->user.value, c->startup.user) != 0) {
			fprintf(stderr, "ignoring new value for `user %s`; retaining old value '%s'\n",
			                p->user.value, c->startup.user);
			free(p->user.value);
			p->user.value = c->startup.user;
		}
	}
	if (p->group.set) {
		if (!reload) {
			c->startup.group = p->group.value;
		} else if (strcmp(p->group.value, c->startup.group) != 0) {
			fprintf(stderr, "ignoring new value for `group %s`; retaining old value '%s'\n",
			                p->group.value, c->startup.group);
			free(p->group.value);
			p->group.value = c->startup.group;
		}
	}

	if (!reload) {
		struct _backend *b;
		c->num_backends = 0;
		for (b = p->backends; b->next; b = b->next) {
			c->num_backends++;
		}
		c->backends = calloc(c->num_backends, sizeof(BACKEND));
		if (!c->backends) {
			return 1;
		}

		int i;
		struct _backend *def = p->backends;
		b = def->next;
		for (i = 0; i < c->num_backends; i++, b = b->next) {
			c->backends[i].hostname = strdup(b->id); /* FIXME! */
			c->backends[i].port = 5432; /* FIXME! */
			c->backends[i].serial++;

			c->backends[i].tls              = get_int(BACKEND_TLS_OFF, &def->tls, &b->tls);
			c->backends[i].health.threshold = get_int(BACKEND_TLS_OFF, &def->lag, &b->lag);
			c->backends[i].weight           = get_int(BACKEND_TLS_OFF, &def->weight, &b->weight);
			c->backends[i].health.database  = get_str("postgres", &p->health_database, NULL);
			c->backends[i].health.username  = get_str("postgres", &p->health_username, NULL);
			c->backends[i].health.password  = get_str("",         &p->health_password, NULL);
		}
	}

	return 0;
}

#ifdef PTEST
int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "USAGE: %s /path/to/config/file\n", argv[0]);
		return 1;
	}

	pgr_logger(LOG_DEBUG);
	pgr_logf(stderr, LOG_INFO, "cfgtest starting up...");

	CONTEXT c;
	memset(&c, 0, sizeof(c));
	if (pgr_configure(&c, argv[1], 0) != 0) {
		fprintf(stderr, "pgr_configure: [%d] %s\n", errno, strerror(errno));
		return 3;
	}

	printf("# pgrouter config\n");
	printf("listen  %s\n", c.startup.frontend);
	printf("monitor %s\n", c.startup.monitor);
	printf("\n");
	printf("hba %s\n", c.startup.hbafile);
	printf("pidfile %s\n", c.startup.pidfile);
	printf("user %s\n", c.startup.user);
	printf("group %s\n", c.startup.group);
	printf("\n");
	printf("workers %d\n", c.workers);
	printf("log %s\n", c.loglevel == LOG_DEBUG ? "DEBUG"
	                 : c.loglevel == LOG_INFO  ? "INFO"  : "ERROR");
	printf("\n");
	printf("tls {\n");
	printf("  ciphers %s\n", c.startup.tls_ciphers);
	printf("  cert    %s\n", c.startup.tls_certfile);
	printf("  key     %s\n", c.startup.tls_keyfile);
	printf("}\n");
	printf("\n");
	printf("health {\n");
	printf("  check    %ds\n", c.health.interval);
	printf("  timeout  %ds\n", c.health.timeout);
	printf("  database %s\n", c.health.database);
	printf("  username %s\n", c.health.username);
	printf("  password %s\n", c.health.password);
	printf("}\n");
	printf("\n");
	int i;
	for (i = 0; i < c.num_backends; i++) {
		BACKEND b = c.backends[i];
		printf("backend %s {\n", b.hostname);
		printf("  tls %s\n", b.tls == BACKEND_TLS_VERIFY   ? "on"
		                   : b.tls == BACKEND_TLS_NOVERIFY ? "skipverify" : "off");
		printf("  weight %d\n", b.weight);
		printf("  lag %llub\n", b.health.threshold);
		printf("}\n");
		printf("\n");
	}

	return 0;
}
#endif
