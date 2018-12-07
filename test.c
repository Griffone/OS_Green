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

static struct {
	int counter[COUNTER_SIZE];	// large structure to increase chances of increment conflict
} common;

void increment_counter() {
	for (int i = 0; i < COUNTER_SIZE; ++i) {
		common.counter[i]++;
	}
}

void validate_counter() {
	int c = common.counter[0];
	int corrupted = 0;
	
	for (int i = 1; i < COUNTER_SIZE; ++i) {
		if (corrupted == 0 && common.counter[i] != c) {
			corrupted = i;
		}
		common.counter[i] = c;
	}
	
	if (corrupted) {
		printf("Counter was corrupted at %d\n", corrupted);
	}
}

int main() {
	green_t g0, g1, g2;
	green_cond_init(&cond);
	
	for (int i = 0; i < COUNTER_SIZE; ++i) {
		common.counter[i] = 0;
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
			increment_counter();
			validate_counter();
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
			increment_counter();
			validate_counter();
		}
	}
}
