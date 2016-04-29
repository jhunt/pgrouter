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

typedef int (*test_runner)(PGconn*);
static struct {
	const char *name;
	test_runner fn;
	int result;
} TESTS[] = {
	{ "Simple SELECT", test_simple_select, 0 },
	{ "Simple INSERT", test_simple_insert, 0 },
};

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

	rc = load_schema(conn);
	if (rc != 0) {
		fprintf(stderr, "FAILED to load testing schema\n");
		return 8;
	}

	nap = 0;
	if (getenv("TEST_SPREAD") != NULL) {
		nap = atoi(getenv("TEST_SPREAD")) * 1000;
	}

	total = sizeof(TESTS) / sizeof(TESTS[0]);
	fprintf(stderr, "found %i test%s total\n", total, total == 1 ? "" : "s");
	for (i = 0; i < total; i++) {
		usleep(nap);
		fprintf(stderr, "Running test suite '%s'\n", TESTS[i].name);
		fprintf(stderr, "----------------------------------\n");
		TESTS[i].result = TESTS[i].fn(conn);
		fprintf(stderr, "----------------------------------\n");
		fprintf(stderr, "(test suite '%s' complete)\n\n", TESTS[i].name);
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
	fprintf(stderr, "Running simple command query\n  `%s`\n", sql);
	r = PQexec(conn, sql);
	if (!r) {
		fprintf(stderr, "out of memory!\n");
		return 0;
	}

	return 1;
}

static int DATA_QUERY(PGconn *conn, PGresult **r, const char *sql)
{
	fprintf(stderr, "Running data query\n  `%s`\n", sql);
	*r = PQexec(conn, sql);
	if (!*r) {
		fprintf(stderr, "out of memory!\n");
		return 0;
	}

	if (PQresultStatus(*r) != PGRES_TUPLES_OK) {
		fprintf(stderr, "query failed: %s\n", PQresultErrorMessage(*r));
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
		fprintf(stderr, "Found %d tuple(s), expected only 1\n", PQntuples(r));
		return TEST_FAIL;
	}

	return TEST_OK;
}
