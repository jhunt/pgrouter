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

#include "pgrouter.h"
#include <stdio.h>
#include <stdlib.h>

void pgr_abort(int code)
{
	switch (code) {
	case ABORT_MEMFAIL:  fprintf(stderr, "MEMORY ALLOCATION FAILURE; ABORTING.\n");     break;
	case ABORT_LOCK:     fprintf(stderr, "THREAD (UN)LOCKING FAILURE; ABORTING.\n");    break;
	case ABORT_NET:      fprintf(stderr, "UNRECOVERABLE NETWORK FAILURE; ABORTING.\n"); break;
	case ABORT_SYSCALL:  fprintf(stderr, "INTERRUPTED DURING SYSCALL; ABORTING.\n");    break;
	case ABORT_RANDFAIL: fprintf(stderr, "RANDOMNESS/ENTROPY FAILURE; ABORTING.\n");    break;
	case ABORT_UNIMPL:   fprintf(stderr, "UNIMPLEMENTED FEAUTRE; ABORTING.\n");         break;
	case ABORT_ABSURD:   fprintf(stderr, "IMPOSSIBLE STATE (FILE A BUG); ABORTING.\n"); break;
	default:             fprintf(stderr, "UNKNOWN FAILURE (FILE A BUG); ABORTING.\n");  break;
	}
	exit(code);
}
