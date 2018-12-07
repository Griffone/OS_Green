#include "green.h"

#include <stdio.h>

#define LOOP_COUNT		10
#define VERBOSE_HUGGER	0
#define SKIP_HUGGER		0
#define COUNTER_SIZE	1000000

int flag = 0;
green_cond_t cond;

void *test(void *arg);

void *hugger(void *arg);

static struct counter {
	int parts[COUNTER_SIZE];	// large structure to increase chances of increment conflict
} safe_counter, unsafe_counter;

green_mutex_t mutex;

static inline void increment_counter(struct counter *counter) {
	for (int i = 0; i < COUNTER_SIZE; ++i) {
		counter->parts[i]++;
	}
}

// Make sure the counter is not corrupted (all parts align)
//
// Returns 0 if counter is valid
// Returns an integer position of corrupted otherwise
int is_corrupted(struct counter *counter) {
	int c = counter->parts[0];
	int corrupted = 0;
	
	for (int i = 1; i < COUNTER_SIZE; ++i) {
		if (corrupted == 0 && counter->parts[i] != c) {
			corrupted = i;
		}
		counter->parts[i] = c;
	}
	
	return corrupted;
}

static inline void do_counters() {
	increment_counter(&unsafe_counter);
	if (is_corrupted(&unsafe_counter))
		printf("Unsafe counter got corrupted!\n");
	green_mutex_lock(&mutex);
	increment_counter(&safe_counter);
	if (is_corrupted(&safe_counter))
		printf("Safe counter gor corrupted!\n");
	green_mutex_unlock(&mutex);
}

int main() {
	green_t g0, g1, g2;
	green_cond_init(&cond);
	green_mutex_init(&mutex);
	
	for (int i = 0; i < COUNTER_SIZE; ++i) {
		safe_counter.parts[i] = unsafe_counter.parts[i] = 0;
	}
	
	int a0 = 0;
	int a1 = 1;
	green_create(&g0, &test, &a0);
	green_create(&g1, &test, &a1);
	green_create(&g2, &hugger, NULL);
	
	green_join(&g0);
	green_join(&g1);
	// trying to join hugger is pointless as it's perpetual loop
	
	printf("done\n");
	return 0;
}

void *test(void *arg) {
	int id = *(int*)arg;
	int loop = LOOP_COUNT;
	while (loop > 0) {
		if (flag == id) {
			printf("thread %d: %d\n", id, loop);
			do_counters();
			loop--;
			flag = (id + 1) % 2;
			//green_yield();
			green_cond_signal(&cond);
		} else {
			green_cond_wait(&cond);
		}
	}
}

void *hugger(void *arg) {
#if SKIP_HUGGER
	return;
#endif
	unsigned int i = 0;
	printf("Running hugger, which doesn't yield!\n");
	while (1) {
		if (++i % 1000000 == 0) {
#if VERBOSE_HUGGER
			printf("Hugger at %i cycles, still no yield!\n", i);
#endif // VERBOSE_HUGGER
			do_counters();
		}
	}
}
