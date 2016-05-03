/*
  Copyright (c) 2016 James Hunt

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libpq-fe.h>

#define TEST_SKIPPED 0
#define TEST_OK      1
#define TEST_FAIL    2
#define TEST_ERROR   3

static const char* result(int type)
{
	switch (type) {
	case TEST_SKIPPED: return "SKIPPED";
	case TEST_OK:      return "OK";
	case TEST_FAIL:    return "FAIL";
	case TEST_ERROR:   return "ERROR";
	default: return "(UNKNOWN)";
	}
}

static int load_schema(PGconn*);

static int test_simple_select(PGconn*);
static int test_simple_insert(PGconn*);
static int test_large_insert(PGconn*);

typedef int (*test_runner)(PGconn*);
static struct {
	const char *name;
	test_runner fn;
	int result;
} TESTS[] = {
	{ "Simple SELECT", test_simple_select, 0 },
	{ "Simple INSERT", test_simple_insert, 0 },
	{ "Large Payload INSERT", test_large_insert, 0 },
};

static FILE *ERROR;

int main(int argc, char **argv)
{
	int rc, i, total, nap;
	PGconn *conn;

	if (argc != 2) {
		fprintf(stderr, "USAGE: %s <dsn>\n\n", argv[0]);
		fprintf(stderr, "  for example: `%s 'host=127.0.0.1 port=5432 user=test dbname=test'`\n", argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		fprintf(stderr, "USAGE: %s <dsn>\n\n", argv[0]);
		fprintf(stderr, "The <dsn> argument is comprised of key=value pairs, separated\n");
		fprintf(stderr, "by whitespace (probably requires quoting in the shell):\n\n");
		fprintf(stderr, "  host      IP address or hostname to connect to\n");
		fprintf(stderr, "  port      TCP port that pgrouter is listening on\n");
		fprintf(stderr, "  user      Postgres user to connect as\n");
		fprintf(stderr, "  password  Password for the given user\n");
		fprintf(stderr, "  dbname    Name of the database to run tests in\n");
		return 0;
	}

	fprintf(stderr, "Connecting with dsn '%s'\n", argv[1]);
	conn = PQconnectdb(argv[1]);
	if (!conn) {
		fprintf(stderr, "FAILED to setup the connection to pgrouter (memory issue)\n");
		return 7;
	}
	if (PQstatus(conn) != CONNECTION_OK) {
		fprintf(stderr, "FAILED to connect to pgrouter: %s\n", PQerrorMessage(conn));
		return 7;
	}

	ERROR = tmpfile();
	if (!ERROR) {
		fprintf(stderr, "FAILED to set up a buffering tempfile for test out: %s\n", strerror(errno));
		return 8;
	}

	rc = load_schema(conn);
	if (rc != 0) {
		fprintf(stderr, "FAILED to load testing schema\n");
		return 9;
	}

	nap = 0;
	if (getenv("TEST_SPREAD") != NULL) {
		nap = atoi(getenv("TEST_SPREAD")) * 1000;
	}

	total = sizeof(TESTS) / sizeof(TESTS[0]);
	fprintf(stderr, "found %i test%s total\n", total, total == 1 ? "" : "s");
	for (i = 0; i < total; i++) {
		usleep(nap);

		ftruncate(fileno(ERROR), 0);
		rewind(ERROR);

		TESTS[i].result = TESTS[i].fn(conn);
		if (TESTS[i].result != TEST_OK) {
			char block[4096];

			fprintf(stderr, "'%s' %s\n", TESTS[i].name, result(TESTS[i].result));
			fprintf(stderr, "----------------------------------\n");

			rewind(ERROR);
			while (fgets(block, 4096, ERROR) != NULL) {
				fprintf(stderr, "%s", block);
			}
			fprintf(stderr, "\n\n\n");
		}
	}

	rc = 0;
	for (i = 0; i < total; i++) {
		fprintf(stdout, "%-40s  %s\n", TESTS[i].name,
			result(TESTS[i].result));
		switch (TESTS[i].result) {
		case TEST_FAIL:  rc = rc > 1 ? rc : 1; break;
		case TEST_ERROR: rc = rc > 2 ? rc : 2; break;
		}
	}
	return rc;
}

static int COMMAND_QUERY(PGconn *conn, const char *sql)
{
	PGresult *r;
	fprintf(ERROR, "Running simple command query\n  `%s`\n", sql);
	r = PQexec(conn, sql);
	if (!r) {
		fprintf(ERROR, "out of memory!\n");
		return 0;
	}

	return 1;
}

static int DATA_QUERY(PGconn *conn, PGresult **r, const char *sql)
{
	fprintf(ERROR, "Running data query\n  `%s`\n", sql);
	*r = PQexec(conn, sql);
	if (!*r) {
		fprintf(ERROR, "out of memory!\n");
		return 0;
	}

	if (PQresultStatus(*r) != PGRES_TUPLES_OK) {
		fprintf(ERROR, "query failed: %s\n", PQresultErrorMessage(*r));
		return 0;
	}

	return 1;
}

static int load_schema(PGconn *conn)
{
	if (!COMMAND_QUERY(conn, "DROP TABLE notes")
	 || !COMMAND_QUERY(conn, "CREATE TABLE notes (id INTEGER, note TEXT)")
	 || !COMMAND_QUERY(conn, "INSERT INTO notes (id, note)"
	                         "  VALUES (1, 'this is the first note')")
	) {
		return 1;
	}

	return 0;
}

static int test_simple_select(PGconn *conn)
{
	PGresult *r;

	if (!DATA_QUERY(conn, &r, "SELECT note FROM notes WHERE id = 1")) {
		return TEST_FAIL;
	}

	return TEST_OK;
}

static int test_simple_insert(PGconn *conn)
{
	PGresult *r;

	if (!COMMAND_QUERY(conn, "INSERT INTO notes (id, note)"
	                         "  VALUES (2, 'another note')")
	 || !DATA_QUERY(conn, &r, "SELECT note FROM notes WHERE id = 2")
	) {
		return TEST_FAIL;
	}

	if (PQntuples(r) != 1 ) {
		fprintf(ERROR, "Found %d tuple(s), expected only 1\n", PQntuples(r));
		return TEST_FAIL;
	}

	return TEST_OK;
}

static int test_large_insert(PGconn *conn)
{
	/* `INSERT INTO notes (id, note) VALUES (3, '')` is 43 characters long */
	char *sql = malloc(65536); /* 64k */
	long unsigned int i;

	for (i = 1 << 8; i <= 65536; i = i << 1) {
		fprintf(ERROR, "Testing a %lub SQL INSERT\n", i);

		fprintf(ERROR, "Deleting previous note record #3...\n");
		if (!COMMAND_QUERY(conn, "DELETE FROM notes WHERE id = 3")) {
			return TEST_FAIL;
		}

		memcpy(sql, "INSERT INTO notes (id, note) VALUES (3, '", 41);
		memset(sql+41, 'a', i - 44);
		memcpy(sql+41+(i-44), "')", 3);
		if (!COMMAND_QUERY(conn, sql)) {
			return TEST_FAIL;
		}
	}
}
