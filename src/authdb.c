#include "pgrouter.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

#define SUBSYS "authdb"
#include "locks.inc.c"

#define C_ALPHA   "abcdefghijklmnopqrstuvwxyz" "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define C_NUMERIC "0123456789"
#define C_ALPHANUMERIC C_ALPHA C_NUMERIC


#define  T_RESTART  256
#define  T_ERROR    257
#define  T_EOS      258
#define  T_WORD     259
#define  T_QSTRING  260
#define  T_NEWLINE  261

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

struct _auth_entry {
	char *username;
	char *password;
	struct _auth_entry *next;
};

struct __p {
	int count;
	struct _auth_entry *entries;

	LEXER *l;      /* lexer to get tokens from */
	parser_fn f;   /* current parser function  */
};

static const char* token_name(int type)
{
	switch (type) {
	case T_RESTART: return "T_RESTART";
	case T_ERROR:   return "T_ERROR";
	case T_EOS:     return "T_EOS";
	case T_WORD:    return "T_WORD";
	case T_QSTRING: return "T_QSTRING";
	case T_NEWLINE: return "T_NEWLINE";
	default: return "(unknown)";
	}
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
	pgr_debugf("got a token {%s(%d), '%s', %d}",
			token_name(t->type), t->type, buf, t->length);
}

static void dump_lexer(LEXER *l)
{
	char buf[256];
	int n = bound(l->pos - l->start, 0, 255);
	strncpy(buf, l->src + l->start, n);
	buf[n] = '\0';
	pgr_debugf("lexer at %s:%d:%d, %d/%d s=%d '%s'",
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
static TOKEN lex_qstring(LEXER *l);

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
			return token(T_NEWLINE, NULL);
		}

		if (isspace(c)) {
			ignore(l);
			continue;
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

		l->f = lex_bareword;
		return TRY_AGAIN;
	}

	l->f = NULL;
	return token(T_EOS, NULL);
}

static TOKEN lex_bareword(LEXER *l)
{
	l->f = lex_any;

	for (;;) {
		char c = next(l);
		if (c == 0 || isspace(c)) {
			backup(l);
			break;
		}
	}

	char *value = l->src + l->start;
	int length  = l->pos - l->start;

	/* nope, return it as a bareword */
	return token(T_WORD, l);
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

	return token(T_QSTRING, l);
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

LEXER* lexer_init(const char *file, FILE *io)
{
	pgr_debugf("initializing a new authdb lexer for %s (io %p)", file, io);

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
	pgr_debugf("looks like there are %lu bytes of data to parse", size);

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
		pgr_debugf("read %u bytes, current position %d/%d, %d bytes left",
				n, pos, size, left);
	}
	pgr_debugf("final position %d/%d, %d bytes left",
			pos, size, left);
	pgr_debugf("src:\n```\n%s```", l->src);
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
	case T_WORD:
		s = calloc(t->length + 1, sizeof(char));
		if (!s) {
			return NULL;
		}
		strncpy(s, t->value, t->length);
		return s;

	case T_QSTRING:
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

static int parse(PARSER *p)
{
	while (p->f && p->f(p) == 0)
		;
	return 0;
}

static void new_entry(PARSER *p, const char *user, const char *pw)
{
	struct _auth_entry *a = calloc(1, sizeof(struct _auth_entry));
	if (!a) {
		fprintf(stderr, "failed to allocate new authdb entry: %s\n", strerror(errno));
		return;
	}
	a->username = strdup(user);
	a->password = strdup(pw);
	a->next = p->entries;
	p->entries = a;
	p->count++;
}

static int parse_top(PARSER *p)
{
	TOKEN t1, t2, t3;
	char *user, *type, *hash;

	t1 = emit(p->l);
	switch (t1.type) {
	case T_NEWLINE:
		break;

	case T_WORD:
	case T_QSTRING:
		user = as_string(&t1);
		if (!user) {
			pgr_abort(ABORT_ABSURD);
		}

		t2 = emit(p->l);
		switch (t2.type) {
		case T_NEWLINE:
			p->f = NULL;
			fprintf(stderr, "unexpected newline after username '%s'\n", user);
			return 1;

		case T_WORD:
			type = as_string(&t2);
			if (!type) {
				pgr_abort(ABORT_ABSURD);
			}
			if (strcmp(type, "md5") != 0) {
				p->f = NULL;
				fprintf(stderr, "invalid account type '%s'\n", type);
				free(user);
				free(type);
				return 1;
			}
			free(type);

			t3 = emit(p->l);
			switch (t3.type) {
			case T_WORD:
			case T_QSTRING:
				hash = as_string(&t3);
				if (!hash) {
					pgr_abort(ABORT_ABSURD);
				}

				new_entry(p, user, hash);
				free(user);
				free(hash);
				break;

			case T_NEWLINE:
				p->f = NULL;
				fprintf(stderr, "unexpected newline after md5 keyword\n");
				return 1;

			case T_ERROR:
				p->f = NULL;
				fprintf(stderr, "unexpected error\n");
				return 1;

			case T_EOS:
				p->f = NULL;
				fprintf(stderr, "unexpected end-of-stream\n");
				return 1;
			}
			break;

		case T_ERROR:
			p->f = NULL;
			fprintf(stderr, "unexpected error\n");
			return 1;

		case T_EOS:
			p->f = NULL;
			fprintf(stderr, "unexpected end-of-stream\n");
			return 0;
		}
		break;

	case T_ERROR:
		p->f = NULL;
		fprintf(stderr, "unexpected error\n");
		return 1;

	case T_EOS:
		p->f = NULL;
		return 0;

	default:
		fprintf(stderr, "bad toplevel\n");
		p->f = NULL;
		return 1;
	}

	return 0;
}

static void parser_free(PARSER *p)
{
	free(p->l->file);
	free(p->l->src);
	free(p->l);

	struct _auth_entry *tmp, *next = p->entries;
	while (next) {
		tmp = next->next;
		free(next->username);
		free(next->password);
		free(next);
		next = tmp;
	}

	free(p);
}

static PARSER* parser_init(const char *file, FILE *io, int reload)
{
	PARSER *p = calloc(1, sizeof(PARSER));
	if (!p) {
		return NULL;
	}

	p->f = parse_top;
	p->l = lexer_init(file, io);
	if (!p->l) {
		free(p);
		return NULL;
	}

	return p;
}

int pgr_authdb(CONTEXT *c, int reload)
{
	int rc;
	FILE *io = fopen(c->authdb.file, "r");
	if (!io) {
		return 1;
	}

	PARSER *p = parser_init(c->authdb.file, io, reload);
	fclose(io);
	if (!p) {
		return 1;
	}

	rc = parse(p);
	if (rc != 0) {
		parser_free(p);
		return rc;
	}

	c->authdb.num_entries = p->count;
	c->authdb.usernames = calloc(p->count, sizeof(char*));
	c->authdb.md5hashes = calloc(p->count, sizeof(char*));
	if (!c->authdb.usernames || !c->authdb.md5hashes) {
		free(c->authdb.usernames);
		free(c->authdb.md5hashes);
		fprintf(stderr, "failed to allocate memory for final authdb entries: %s\n", strerror(errno));
		parser_free(p);
		return -1;
	}

	int i;
	struct _auth_entry *entry;
	for (i = c->authdb.num_entries - 1, entry = p->entries;
	     i >= 0 && entry;
	     i--, entry = entry->next) {
		c->authdb.usernames[i] = entry->username;
		c->authdb.md5hashes[i] = entry->password;
		entry->username = entry->password = NULL;
	}

	parser_free(p);
	return 0;
}

const char* pgr_auth_find(CONTEXT *c, const char *username)
{
	int rc, i;
	const char *md5;

	rdlock(&c->lock, "context", 0);

	md5 = NULL;
	for (i = 0; i < c->authdb.num_entries; i++) {
		if (strcmp(c->authdb.usernames[i], username) == 0) {
			md5 = c->authdb.md5hashes[i];
			break;
		}
	}

	unlock(&c->lock, "context", 0);
	return md5;
}

#if defined(PTEST)
static char *quote(const char *s)
{
	int len = 0;
	const char *p;

	for (p = s; *p; p++) {
		if (*p == '"') {
			len++;
		}
		len++;
	}

	char *qq = calloc(len + 1, sizeof(char));
	if (!qq) {
		return NULL;
	}

	char *q;
	for (p = s, q = qq; *p; p++) {
		if (*p == '"') {
			*q++ = '\\';
		}
		*q++ = *p;
	}

	return qq;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "USAGE: %s /path/to/authdb\n", argv[0]);
		return 1;
	}

	pgr_logger(LOG_DEBUG);
	pgr_logf(stderr, LOG_INFO, "authdbtest starting up...");

	CONTEXT c;
	memset(&c, 0, sizeof(c));
	c.authdb.file = argv[1];
	if (pgr_authdb(&c, 0) != 0) {
		fprintf(stderr, "pgr_authdb: [%d] %s\n", errno, strerror(errno));
		return 3;
	}

	printf("# pgrouter authdb\n");
	int i;
	for (i = 0; i < c.authdb.num_entries; i++) {
		int ws = 0;
		char *s;
		for (s = c.authdb.md5hashes[i]; *s; s++) {
			if (isspace(*s)) {
				ws = 1;
				break;
			}
		}
		if (ws) {
			char *quoted = quote(c.authdb.md5hashes[i]);
			printf("%s md5 \"%s\"\n", c.authdb.usernames[i], quoted);
			free(quoted);
		} else {
			printf("%s md5 %s\n", c.authdb.usernames[i], c.authdb.md5hashes[i]);
		}
	}

	return 0;
}
#elif defined(ATEST)
int main(int argc, char **argv)
{
	int rc;
	CONTEXT c;
	MD5 md5;
	char hash[33], *buf;
	const char *check;

	if (argc != 4) {
		fprintf(stderr, "USAGE: %s /path/to/authdb user password\n", argv[0]);
		return 0;
	}

	rc = pgr_context(&c);
	if (rc != 0) {
		fprintf(stderr, "failed to initialize global context\n");
		return 1;
	}
	pgr_md5_init(&md5);

	c.authdb.file = argv[1];
	rc = pgr_authdb(&c, 0);
	if (rc != 0) {
		fprintf(stderr, "failed to read authdb %s\n", c.authdb.file);
		return 1;
	}

	check = pgr_auth_find(&c, argv[2]);
	if (check == NULL) {
		fprintf(stderr, "%s: no such user\n", argv[2]);
		return 1;
	}

	rc = asprintf(&buf, "%s%s", argv[3], argv[2]);
	if (rc == -1) {
		fprintf(stderr, "failed to create pre-hash string buffer\n");
		return 1;
	}

	pgr_md5_update(&md5, buf, strlen(buf));
	pgr_md5_hex(hash, &md5);
	if (strncmp(hash, check, 32) == 0) {
		printf("%s OK\n", argv[2]);
		return 0;
	}

	fprintf(stderr, "%s: authentication failed\n", argv[2]);
	return 1;
}
#endif
