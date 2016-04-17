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
	default:             fprintf(stderr, "UNKNOWN FAILURE (FILE A BUG); ABORTING.\n");  break;
	}
	exit(code);
}
