#include "green.h"

#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

#define ENABLE_GREEN		1
#define ENABLE_PTHREAD		1

#define ENABLE_INDEPENDENT	1
#define ENABLE_ORDERED		0
#define ENABLE_SYNCHRONIZED	1

#define THREAD_COUNT	8
#define CYCLE_COUNT		1000000

#define TU_PER_SEC		1000
#define NANOS_PER_TU	(1000000000 / TU_PER_SEC)

static int counters[THREAD_COUNT];
static int shared_counter;
static int flag;

static char const * const TIME_UNIT = "ms";

static inline double get_time_since(struct timespec *time) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double seconds = now.tv_sec - time->tv_sec;
	long nanos = (now.tv_nsec + (1000000000 - time->tv_nsec)) % 1000000000;
	return seconds / TU_PER_SEC + (double) nanos / NANOS_PER_TU;
}

enum Library {Green, Pthread};

typedef struct synchronization {
	enum Library	library;
	union {
		struct {
			green_mutex_t	mutex;
			green_cond_t	cond;
		} green;
		struct {
			pthread_mutex_t	mutex;
			pthread_cond_t	cond;
		} pthread;
	} objects;
} synchronization;

struct thread_args {
	int				id;
	synchronization	*sync;
};

/// Some task that performs an independent calculation on a thread-owned data structure
void *independent(void *arg);

/// Tasks that are meant to be completed in certain order
void *ordered(void *arg);

/// Some task that performs a workload that needs to be synchronized with others to work as expected
void *synchronized(void *arg);

int main() {
	green_t				gthreads[THREAD_COUNT];
	pthread_t			pthreads[THREAD_COUNT];
	struct thread_args	args[THREAD_COUNT];
	
	synchronization sync = {0};
	double greens[3], pts[3];
	
	sync.library = Green;
	struct timespec start_time;
	
	for (int i = 0; i < THREAD_COUNT; ++i) {
		args[i].id = i;
		args[i].sync = &sync;
		counters[i] = 0;
	}
	
#if ENABLE_GREEN
#if ENABLE_INDEPENDENT
	printf("Running %d green independent tasks\n", THREAD_COUNT);
	clock_gettime(CLOCK_MONOTONIC, &start_time);
	for (int i = 0; i < THREAD_COUNT; ++i) green_create(&gthreads[i], independent, &counters[i]);
	for (int i = 0; i < THREAD_COUNT; ++i) green_join(&gthreads[i]);
	greens[0] = get_time_since(&start_time);
	printf("%d green independent tasks finished in %f%s\n", THREAD_COUNT, greens[0], TIME_UNIT);
#endif // ENABLE_INDEPENDENT

#if ENABLE_ORDERED
	shared_counter = 0;
	flag = 0;
	green_mutex_init(&sync.objects.green.mutex);
	green_cond_init(&sync.objects.green.cond);
	
	printf("Running %d green ordered tasks\n", THREAD_COUNT);
	clock_gettime(CLOCK_MONOTONIC, &start_time);
	for (int i = 0; i < THREAD_COUNT; ++i) green_create(&gthreads[i], ordered, &args[i]);
	for (int i = 0; i < THREAD_COUNT; ++i) green_join(&gthreads[i]);
	greens[1] = get_time_since(&start_time);
	printf("%d green ordered tasks finished in %f%s\n", THREAD_COUNT, greens[1], TIME_UNIT);
	assert(shared_counter == THREAD_COUNT * CYCLE_COUNT);
#endif // ENABLE_ORDERED

#if ENABLE_SYNCHRONIZED
	shared_counter = 0;
	green_mutex_init(&sync.objects.green.mutex);
	
	printf("Running %d green synchronized tasks\n", THREAD_COUNT);
	clock_gettime(CLOCK_MONOTONIC, &start_time);
	for (int i = 0; i < THREAD_COUNT; ++i) green_create(&gthreads[i], synchronized, &sync);
	for (int i = 0; i < THREAD_COUNT; ++i) green_join(&gthreads[i]);
	greens[2] = get_time_since(&start_time);
	printf("%d green synchronized tasks finished in %f%s\n", THREAD_COUNT, greens[2], TIME_UNIT);
	assert(shared_counter == THREAD_COUNT * CYCLE_COUNT);
#endif // ENABLE_SYNCHRONIZED
#endif // ENABLE_GREEN

#if ENABLE_PTHREAD
	sync.library = Pthread;
	
#if ENABLE_INDEPENDENT
	for (int i = 0; i < THREAD_COUNT; ++i) counters[i] = 0;
	
	printf("Running %d pthread independent tasks\n", THREAD_COUNT);
	clock_gettime(CLOCK_MONOTONIC, &start_time);
	for (int i = 0; i < THREAD_COUNT; ++i) pthread_create(&pthreads[i], NULL, independent, &counters[i]);
	for (int i = 0; i < THREAD_COUNT; ++i) pthread_join(pthreads[i], NULL);
	pts[0] = get_time_since(&start_time);
	printf("%d pthread independent tasks finished in %f%s\n", THREAD_COUNT, pts[0], TIME_UNIT);
#endif // ENABLE_INDEPENDENT
	
#if ENABLE_ORDERED
	shared_counter = 0;
	flag = 0;
	pthread_mutex_init(&sync.objects.pthread.mutex, NULL);
	pthread_cond_init(&sync.objects.pthread.cond, NULL);
	
	printf("Running %d pthread ordered tasks\n", THREAD_COUNT);
	clock_gettime(CLOCK_MONOTONIC, &start_time);
	for (int i = 0; i < THREAD_COUNT; ++i) pthread_create(&pthreads[i], NULL, ordered, &args[i]);
	for (int i = 0; i < THREAD_COUNT; ++i) pthread_join(pthreads[i], NULL);
	pts[1] = get_time_since(&start_time);
	printf("%d pthread ordered tasks finished in %f%s\n", THREAD_COUNT, pts[1], TIME_UNIT);
	//assert(shared_counter == THREAD_COUNT * CYCLE_COUNT);
#endif // ENABLE_ORDERED
	
#if ENABLE_SYNCHRONIZED
	pthread_mutex_init(&sync.objects.pthread.mutex, NULL);
	shared_counter = 0;
	
	printf("Running %d pthread synchronized tasks\n", THREAD_COUNT);
	clock_gettime(CLOCK_MONOTONIC, &start_time);
	for (int i = 0; i < THREAD_COUNT; ++i) pthread_create(&pthreads[i], NULL, synchronized, &sync);
	for (int i = 0; i < THREAD_COUNT; ++i) pthread_join(pthreads[i], NULL);
	pts[2] = get_time_since(&start_time);
	printf("%d pthread synchronized tasks finished in %f%s\n", THREAD_COUNT, pts[2], TIME_UNIT);
	assert(shared_counter == THREAD_COUNT * CYCLE_COUNT);
#endif // ENABLE_SYNCHRONIZED
#endif // ENABLE_PTHREAD

#if ENABLE_GREEN || ENABLE_PTHREAD
	printf("\n                RESULTS:\n");
	printf("    test     ||  green   ||   pthread\n");
#if ENABLE_INDEPENDENT
	printf(" independent ||%8.2f%s||%8.2f%s\n", greens[0], TIME_UNIT, pts[0], TIME_UNIT);
#endif // ENABLE_INDEPENDENT
#if ENABLE_ORDERED
	printf("   ordered   ||%8.2f%s||%8.2f%s\n", greens[1], TIME_UNIT, pts[1], TIME_UNIT);
#endif // ENABLE_ORDERED
#if ENABLE_SYNCHRONIZED
	printf("synchronized ||%8.2f%s||%8.2f%s\n", greens[2], TIME_UNIT, pts[2], TIME_UNIT);
#endif // ENABLE_SYNCHRONIZED
#endif // ENABLE_GREEN || ENABLE_PTHREAD
	
	printf("done\n");
}

void cond_wait_green(void *cond, void *mutex) {
	green_cond_wait((green_cond_t *)cond, (green_mutex_t *)mutex);
}

void cond_wait_pthread(void *cond, void *mutex) {
	pthread_cond_wait((pthread_cond_t *)cond, (pthread_mutex_t *)mutex);
}

void cond_signal_green(void *cond) {
	green_cond_signal((green_cond_t *)cond);
}

void cond_signal_pthread(void *cond) {
	pthread_cond_signal((pthread_cond_t *)cond);
}

void mutex_lock_green(void *mutex) {
	green_mutex_lock((green_mutex_t *)mutex);
}

void mutex_lock_pthread(void *mutex) {
	pthread_mutex_lock((pthread_mutex_t *)mutex);
}

void mutex_unlock_green(void *mutex) {
	green_mutex_unlock((green_mutex_t *)mutex);
}

void mutex_unlock_pthread(void *mutex) {
	pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

void *independent(void *arg) {
	int *counter = (int *)arg;
	
	for (int cycle = 0; cycle < CYCLE_COUNT; ++cycle) {
		*counter++;
	}
}

void *ordered(void *arg) {
	int id = ((struct thread_args *)arg)->id;
	synchronization *sync = ((struct thread_args *)arg)->sync;
	void(*cond_wait)(void *cond, void*mutex);
	void(*cond_signal)(void *cond);
	void(*mutex_lock)(void *mutex);
	void(*mutex_unlock)(void *mutex);
	void *mutex, *cond;
	if (sync->library == Green) {
		cond_wait = &cond_wait_green;
		cond_signal = &cond_signal_green;
		mutex_lock = &mutex_lock_green;
		mutex_unlock = &mutex_unlock_green;
		mutex = &sync->objects.green.mutex;
		cond = &sync->objects.green.cond;
	} else {
		cond_wait = &cond_wait_pthread;
		cond_signal = &cond_signal_pthread;
		mutex_lock = &mutex_lock_pthread;
		mutex_unlock = &mutex_unlock_pthread;
		mutex = &sync->objects.pthread.mutex;
		cond = &sync->objects.pthread.cond;
	}
	
	// This test leads to deadlocking, so I disabled it for now
	for (int cycle = 0; cycle < CYCLE_COUNT; ++cycle) {
		mutex_lock(mutex);
		while (flag != id) {
			cond_wait(cond, mutex);
		}
		printf("(%d) flagging %d\n", id, flag);
		flag = (flag + 1) % THREAD_COUNT;
		shared_counter++;
		cond_signal(cond);
		mutex_unlock(mutex);
	}
}

void *synchronized(void *arg) {
	synchronization *sync = (synchronization *)arg;
	void(*cond_wait)(void *cond, void*mutex);
	void(*cond_signal)(void *cond);
	void(*mutex_lock)(void *mutex);
	void(*mutex_unlock)(void *mutex);
	void *mutex, *cond;
	if (sync->library == Green) {
		cond_wait = &cond_wait_green;
		cond_signal = &cond_signal_green;
		mutex_lock = &mutex_lock_green;
		mutex_unlock = &mutex_unlock_green;
		mutex = &sync->objects.green.mutex;
		cond = &sync->objects.green.cond;
	} else {
		cond_wait = &cond_wait_pthread;
		cond_signal = &cond_signal_pthread;
		mutex_lock = &mutex_lock_pthread;
		mutex_unlock = &mutex_unlock_pthread;
		mutex = &sync->objects.pthread.mutex;
		cond = &sync->objects.pthread.cond;
	}
	
	for (int cycle = 0; cycle < CYCLE_COUNT; ++cycle) {
		mutex_lock(mutex);
		shared_counter++;
		mutex_unlock(mutex);
	}
}
