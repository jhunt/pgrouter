#define T_EOS                    256
#define T_RESTART                257
#define T_ERROR                  258
#define T_OPEN                   259
#define T_CLOSE                  260
#define T_TERMX                  261
#define T_KEYWORD_AUTHDB         262
#define T_KEYWORD_BACKEND        263
#define T_KEYWORD_CERT           264
#define T_KEYWORD_CHECK          265
#define T_KEYWORD_CIPHERS        266
#define T_KEYWORD_DATABASE       267
#define T_KEYWORD_DEBUG          268
#define T_KEYWORD_DEFAULT        269
#define T_KEYWORD_ERROR          270
#define T_KEYWORD_GROUP          271
#define T_KEYWORD_HBA            272
#define T_KEYWORD_HEALTH         273
#define T_KEYWORD_INFO           274
#define T_KEYWORD_KEY            275
#define T_KEYWORD_LAG            276
#define T_KEYWORD_LISTEN         277
#define T_KEYWORD_LOG            278
#define T_KEYWORD_MONITOR        279
#define T_KEYWORD_OFF            280
#define T_KEYWORD_ON             281
#define T_KEYWORD_PASSWORD       282
#define T_KEYWORD_PIDFILE        283
#define T_KEYWORD_SKIPVERIFY     284
#define T_KEYWORD_TIMEOUT        285
#define T_KEYWORD_TLS            286
#define T_KEYWORD_USER           287
#define T_KEYWORD_USERNAME       288
#define T_KEYWORD_WEIGHT         289
#define T_KEYWORD_WORKERS        290
#define T_TYPE_BAREWORD          291
#define T_TYPE_DECIMAL           292
#define T_TYPE_INTEGER           293
#define T_TYPE_ADDRESS           294
#define T_TYPE_TIME              295
#define T_TYPE_SIZE              296
#define T_TYPE_QSTRING           297

/* keyword lookup table */
static struct {
	int         value;
	const char *match;
} KEYWORDS[] = {
	{ T_KEYWORD_AUTHDB,        "authdb"        },
	{ T_KEYWORD_BACKEND,       "backend"       },
	{ T_KEYWORD_CERT,          "cert"          },
	{ T_KEYWORD_CHECK,         "check"         },
	{ T_KEYWORD_CIPHERS,       "ciphers"       },
	{ T_KEYWORD_DATABASE,      "database"      },
	{ T_KEYWORD_DEBUG,         "debug"         },
	{ T_KEYWORD_DEFAULT,       "default"       },
	{ T_KEYWORD_ERROR,         "error"         },
	{ T_KEYWORD_GROUP,         "group"         },
	{ T_KEYWORD_HBA,           "hba"           },
	{ T_KEYWORD_HEALTH,        "health"        },
	{ T_KEYWORD_INFO,          "info"          },
	{ T_KEYWORD_KEY,           "key"           },
	{ T_KEYWORD_LAG,           "lag"           },
	{ T_KEYWORD_LISTEN,        "listen"        },
	{ T_KEYWORD_LOG,           "log"           },
	{ T_KEYWORD_MONITOR,       "monitor"       },
	{ T_KEYWORD_OFF,           "off"           },
	{ T_KEYWORD_ON,            "on"            },
	{ T_KEYWORD_PASSWORD,      "password"      },
	{ T_KEYWORD_PIDFILE,       "pidfile"       },
	{ T_KEYWORD_SKIPVERIFY,    "skipverify"    },
	{ T_KEYWORD_TIMEOUT,       "timeout"       },
	{ T_KEYWORD_TLS,           "tls"           },
	{ T_KEYWORD_USER,          "user"          },
	{ T_KEYWORD_USERNAME,      "username"      },
	{ T_KEYWORD_WEIGHT,        "weight"        },
	{ T_KEYWORD_WORKERS,       "workers"       },
	{-1, NULL},
};

/* token const lookup table */
static struct {
	int         value;
	const char *name;
	const char *literal;
} TOKEN_NAMES[] = {
	{ T_EOS,                   "T_EOS",                 NULL            },
	{ T_RESTART,               "T_RESTART",             NULL            },
	{ T_ERROR,                 "T_ERROR",               NULL            },
	{ T_OPEN,                  "T_OPEN",                NULL            },
	{ T_CLOSE,                 "T_CLOSE",               NULL            },
	{ T_TERMX,                 "T_TERMX",               NULL            },
	{ T_KEYWORD_AUTHDB,        "T_KEYWORD_AUTHDB",      "authdb"        },
	{ T_KEYWORD_BACKEND,       "T_KEYWORD_BACKEND",     "backend"       },
	{ T_KEYWORD_CERT,          "T_KEYWORD_CERT",        "cert"          },
	{ T_KEYWORD_CHECK,         "T_KEYWORD_CHECK",       "check"         },
	{ T_KEYWORD_CIPHERS,       "T_KEYWORD_CIPHERS",     "ciphers"       },
	{ T_KEYWORD_DATABASE,      "T_KEYWORD_DATABASE",    "database"      },
	{ T_KEYWORD_DEBUG,         "T_KEYWORD_DEBUG",       "debug"         },
	{ T_KEYWORD_DEFAULT,       "T_KEYWORD_DEFAULT",     "default"       },
	{ T_KEYWORD_ERROR,         "T_KEYWORD_ERROR",       "error"         },
	{ T_KEYWORD_GROUP,         "T_KEYWORD_GROUP",       "group"         },
	{ T_KEYWORD_HBA,           "T_KEYWORD_HBA",         "hba"           },
	{ T_KEYWORD_HEALTH,        "T_KEYWORD_HEALTH",      "health"        },
	{ T_KEYWORD_INFO,          "T_KEYWORD_INFO",        "info"          },
	{ T_KEYWORD_KEY,           "T_KEYWORD_KEY",         "key"           },
	{ T_KEYWORD_LAG,           "T_KEYWORD_LAG",         "lag"           },
	{ T_KEYWORD_LISTEN,        "T_KEYWORD_LISTEN",      "listen"        },
	{ T_KEYWORD_LOG,           "T_KEYWORD_LOG",         "log"           },
	{ T_KEYWORD_MONITOR,       "T_KEYWORD_MONITOR",     "monitor"       },
	{ T_KEYWORD_OFF,           "T_KEYWORD_OFF",         "off"           },
	{ T_KEYWORD_ON,            "T_KEYWORD_ON",          "on"            },
	{ T_KEYWORD_PASSWORD,      "T_KEYWORD_PASSWORD",    "password"      },
	{ T_KEYWORD_PIDFILE,       "T_KEYWORD_PIDFILE",     "pidfile"       },
	{ T_KEYWORD_SKIPVERIFY,    "T_KEYWORD_SKIPVERIFY",  "skipverify"    },
	{ T_KEYWORD_TIMEOUT,       "T_KEYWORD_TIMEOUT",     "timeout"       },
	{ T_KEYWORD_TLS,           "T_KEYWORD_TLS",         "tls"           },
	{ T_KEYWORD_USER,          "T_KEYWORD_USER",        "user"          },
	{ T_KEYWORD_USERNAME,      "T_KEYWORD_USERNAME",    "username"      },
	{ T_KEYWORD_WEIGHT,        "T_KEYWORD_WEIGHT",      "weight"        },
	{ T_KEYWORD_WORKERS,       "T_KEYWORD_WORKERS",     "workers"       },
	{ T_TYPE_BAREWORD,         "T_TYPE_BAREWORD",       NULL            },
	{ T_TYPE_DECIMAL,          "T_TYPE_DECIMAL",        NULL            },
	{ T_TYPE_INTEGER,          "T_TYPE_INTEGER",        NULL            },
	{ T_TYPE_ADDRESS,          "T_TYPE_ADDRESS",        NULL            },
	{ T_TYPE_TIME,             "T_TYPE_TIME",           NULL            },
	{ T_TYPE_SIZE,             "T_TYPE_SIZE",           NULL            },
	{ T_TYPE_QSTRING,          "T_TYPE_QSTRING",        NULL            },
	{-1, NULL, NULL},
};
