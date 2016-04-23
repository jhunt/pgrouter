#include "pgrouter.h"
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>

static pthread_key_t seed;
static pthread_once_t seed_once = PTHREAD_ONCE_INIT;

static void make_seed_key()
{
	pthread_key_create(&seed, free);
}

static int* prng_seed()
{
	int *SEED;
	pthread_once(&seed_once, make_seed_key);
	SEED = pthread_getspecific(seed);
	if (SEED == NULL) {
		SEED = malloc(sizeof(int));
		if (!SEED) {
			pgr_logf(stderr, LOG_ERR, "[rand] unable to allocate memory for PRNG seed: %s (errno %d)",
					strerror(errno), errno);
			pgr_abort(ABORT_MEMFAIL);
		}

		int fd = open(RAND_DEVICE, O_RDONLY);
		if (fd < 0) {
			pgr_logf(stderr, LOG_ERR, "[rand] unable to open %s for reading: %s (errno %d)",
					RAND_DEVICE, strerror(errno), errno);
			pgr_abort(ABORT_RANDFAIL);
		}

		if (read(fd, SEED, sizeof(int)) != sizeof(int)) {
			pgr_logf(stderr, LOG_ERR, "[rand] unable to initialize PRNG from %s: %s (errno %d)",
					RAND_DEVICE, strerror(errno), errno);
			pgr_abort(ABORT_RANDFAIL);
		}
		close(fd);

		pthread_setspecific(seed, SEED);
	}

	return SEED;
}
int pgr_rand(int start, int end)
{
	return (int)((rand_r(prng_seed()) * 1.0 / RAND_MAX) * (end - start) + start + 0.5);
}

void pgr_srand(int x)
{
	prng_seed();
}
