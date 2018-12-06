#include <ucontext.h>

/// Thread information structure
///
/// Contains all necessary info for a given thread
/// Avoid modifying it outside of the library
/// Use green_create() function to initialize
typedef struct green_t {
	ucontext_t *context;
	
	void *(*func)(void *);
	void *arg;
	
	struct green_t *next;
	struct green_t *join;
	
	int zombie;
} green_t;

/// Conditional variable structure
///
/// Contains all necessary info for a given conditional variable
/// Avoid modifying it outside of the library
/// Use provided functions to work with the variable
typedef struct green_cond_t {
	struct green_t *front, *back;
} green_cond_t;

/// Create and start execution of a new thread
///
/// Attempts to mirror pthread_create() functionality
int green_create(green_t *thread, void *(*func)(void *), void *agc);

/// Yield current execution and let a different thread execute
int green_yield();

/// Wait for a given thread to finish execution
int green_join(green_t *);

/// Initialize a conditional variable
void green_cond_init(green_cond_t *);

/// Suspend the current thread on the condition
void green_cond_wait(green_cond_t *);

/// Signal a thread on condition
///
/// Similar to pthread_cond_signal has no effect if no thread is currently waiting on condition
void green_cond_signal(green_cond_t *);