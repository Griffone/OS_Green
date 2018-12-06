#include "green.h"

#include <assert.h>
#include <stdlib.h>
#include <ucontext.h>

#define FALSE		0
#define TRUE		1

#define STACK_SIZE	4096

// These are safety marks, disabling this saves 1 cycle per function each
// (assert might be more), but enabling this makes debugging easier
#define CLEAN_NEXT			1	// Make sure the green_t.next is NULL on a freshly popped thread
#define NON_EMPTY_ASSERT	1	// Check queue emptiness on popping

static ucontext_t	main_context = {0};
static green_t		main_green = {&main_context, NULL, NULL, NULL, NULL, FALSE};

static green_t		*running = &main_green;

static struct {
	struct green_t *front, *back;
} ready_queue;

static inline void push_ready_queue(green_t *thread) {
	if (ready_queue.back != NULL) {
		ready_queue.back->next = thread;
		ready_queue.back = thread;
	} else {
		ready_queue.back = ready_queue.front = thread;
	}
}

static inline green_t *pop_ready_queue() {
#if NON_EMPTY_ASSERT
	assert(ready_queue.front != NULL);
#endif // NON_EMPTY_ASSERT
	green_t *thread = ready_queue.front;
	ready_queue.front = thread->next;
	
	if (thread->next == NULL) ready_queue.back = NULL;
	
#if CLEAN_NEXT
	thread->next = NULL;
#endif // CLEAN_NEXT
	
	return thread;
}

static inline void push_waiting(green_t *waited, green_t *waiter) {
	// Make sure the wait-sequence makes sense
	while (waited->join) {
		waited = waited->join;
	}
	
	waited->join = waiter;
}

static void init()	__attribute__((constructor));	// why hate on C, only any library you add can have an invisible initialize function

void init() {
	getcontext(&main_context);
	
	ready_queue.front = ready_queue.back = NULL;
}

void green_thread() {
	green_t *this = running;
	
	(*this->func)(this->arg);	// execute user function
	
	// Place waiting thread to the ready queue
	if (this->join) {
		push_ready_queue(this->join);
	}
	
	// Free allocated memory
	free(this->context->uc_stack.ss_sp);
	free(this->context);
	
	// this thread is now a zombie
	this->zombie = TRUE;
	
	running = pop_ready_queue();
	setcontext(running->context);
}

int green_create(green_t *new, void *(*func)(void *), void *arg) {
	ucontext_t *context = (ucontext_t *)malloc(sizeof(ucontext_t));
	getcontext(context);
	
	void *stack = malloc(STACK_SIZE);	// hmm the context example allocated stack on the stack for some reason
	
	// We need to initialize uc_stack struct of the context before calling makecontext
	context->uc_stack.ss_sp = stack;
	context->uc_stack.ss_size = STACK_SIZE;
	
	makecontext(
		context,		// The context to modify
		green_thread,	// The function that is called when the thread activates
		0);				// number of int arguments to the above function
	
	// Initialize the thread structure
	new->context = context;
	new->func = func;
	new->arg = arg;
	new->next = NULL;
	new->join = NULL;
	new->zombie = FALSE;
	
	push_ready_queue(new);
	
	return 0;
}

int green_yield() {
	green_t *suspended = running;
	
	push_ready_queue(suspended);
	
	running = pop_ready_queue();
	swapcontext(suspended->context, running->context);
	
	return 0;
}

int green_join(green_t *thread) {
	if (thread->zombie) return 0;
	
	green_t *suspended = running;
	
	push_waiting(thread, suspended);
	
	running = pop_ready_queue();
	swapcontext(suspended->context, running->context);
	
	return 0;
}

/// Push a given thread to wait on conditional variable
static inline void push_cond(green_cond_t *condition, green_t *thread) {
	if (condition->back != NULL) {
		condition->back->next = thread;
		condition->back = thread;
	} else {
		condition->back = condition->front = thread;
	}
}

/// Pop a thread from a conditional variable
static inline green_t *pop_cond(green_cond_t *condition) {
	green_t *thread = condition->front;
	condition->front = thread->next;
	
	if (thread->next == NULL) condition->back = NULL;
	
#if CLEAN_NEXT
	thread->next = NULL;
#endif // CLEAN_NEXT

	return thread;
}

void green_cond_init(green_cond_t *condition) {
	condition->front = condition->back = NULL;	// initially there are no waiting threads
}

void green_cond_wait(green_cond_t *condition) {
	green_t *suspended = running;
	push_cond(condition, suspended);
	
	running = pop_ready_queue();
	swapcontext(suspended->context, running->context);
}

void green_cond_signal(green_cond_t *condition) {
	if (condition->front == NULL) return;

	// Fairly straight forward
	// Although user might signal a condition that no thread is waiting on...
	push_ready_queue(pop_cond(condition));
}
