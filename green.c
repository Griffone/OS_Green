#include "green.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

#include <sys/time.h>

#define FALSE		0
#define TRUE		1

#define PERIOD		100

#define STACK_SIZE	4096

// These are safety marks, disabling this saves 1 cycle per function each
// (assert might be more), but enabling this makes debugging easier
#define CLEAN_NEXT			1	// Make sure the green_t.next is NULL on a freshly popped thread
#define NON_EMPTY_ASSERT	1	// Check queue emptiness on popping

static ucontext_t	main_context = {0};
static green_t		main_green = {&main_context, NULL, NULL, NULL, NULL, FALSE};

static sigset_t		block;

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

void timer_handler(int);

static void init()	__attribute__((constructor));	// why hate on C, only any library you add can have an invisible initialize function

void init() {
	getcontext(&main_context);
	
	ready_queue.front = ready_queue.back = NULL;
	
	// Initialize scheduler timer
	sigemptyset(&block);
	sigaddset(&block, SIGVTALRM);
	
	struct sigaction action = {0};
	struct timeval interval;
	struct itimerval period;
	
	action.sa_handler = timer_handler;
	int result = sigaction(
		SIGVTALRM,	// the signal for which to set the action
		&action,	// the action to call for the signal
		NULL);		// the out pointer to store old action
		
	assert(result == 0);
	
	interval.tv_sec = 0;
	interval.tv_usec = PERIOD;
	
	period.it_interval = interval;
	period.it_value = interval;
	setitimer(
		ITIMER_VIRTUAL,	// use user-time
		&period,		// the new itimerval to set to
		NULL);			// the out pointer for old itimerval
}

void green_thread() {
	green_t *this = running;
	
	(*this->func)(this->arg);	// execute user function
	
	// We are in key area, so make sure not to corrupt any queues
	// Place waiting thread to the ready queue
	sigprocmask(SIG_BLOCK, &block, NULL);
	while (this->join != NULL) {
		push_ready_queue(this->join);
		this->join = this->join->next;
	}
	sigprocmask(SIG_UNBLOCK, &block, NULL);
	
	// Free allocated memory
	free(this->context->uc_stack.ss_sp);
	free(this->context);
	
	// this thread is now a zombie
	this->zombie = TRUE;
	
	sigprocmask(SIG_BLOCK, &block, NULL);
	running = pop_ready_queue();
	setcontext(running->context);
	sigprocmask(SIG_UNBLOCK, &block, NULL);
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
	
	sigprocmask(SIG_BLOCK, &block, NULL);
	push_ready_queue(new);
	sigprocmask(SIG_UNBLOCK, &block, NULL);
	
	return 0;
}

int green_yield() {
	sigprocmask(SIG_BLOCK, &block, NULL);
	green_t *suspended = running;
	
	push_ready_queue(suspended);
	
	running = pop_ready_queue();
	swapcontext(suspended->context, running->context);
	sigprocmask(SIG_UNBLOCK, &block, NULL);
	
	return 0;
}

int green_join(green_t *thread) {
	if (thread->zombie) return 0;
	
	sigprocmask(SIG_BLOCK, &block, NULL);
	green_t *suspended = running;
	
	// This is smart
	// Make next null if we're the only ones waiting
	// Make next something if someone is already waiting
	suspended->next = thread->join;
	thread->join = suspended;
	
	running = pop_ready_queue();
	swapcontext(suspended->context, running->context);
	sigprocmask(SIG_UNBLOCK, &block, NULL);
	
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
	sigprocmask(SIG_BLOCK, &block, NULL);
	green_t *suspended = running;
	push_cond(condition, suspended);
	
	running = pop_ready_queue();
	swapcontext(suspended->context, running->context);
	sigprocmask(SIG_UNBLOCK, &block, NULL);
}

void green_cond_signal(green_cond_t *condition) {
	if (condition->front == NULL) return;

	// Fairly straight forward
	// Although user might signal a condition that no thread is waiting on...
	sigprocmask(SIG_BLOCK, &block, NULL);
	push_ready_queue(pop_cond(condition));
	sigprocmask(SIG_UNBLOCK, &block, NULL);
}

void timer_handler(int sig) {
	green_t *suspended = running;
	
	push_ready_queue(suspended);
	
	running = pop_ready_queue();
	swapcontext(suspended->context, running->context);
}

void green_mutex_init(green_mutex_t *mutex) {
	mutex->taken = FALSE;
	mutex->suspended = NULL;
}

int green_mutex_lock(green_mutex_t *mutex) {
	sigprocmask(SIG_BLOCK, &block, NULL);
	
	green_t *suspended = running;
	while (mutex->taken) {
		if (mutex->suspended != NULL) {
			// find end of queue
			green_t *last = mutex->suspended;
			while (last->next != NULL) last = last->next;
			
			last->next = suspended;
		} else {
			// Get first in queue
			mutex->suspended = suspended;
		}
		suspended->next = NULL;
		
		running = pop_ready_queue();
		swapcontext(suspended->context, running->context);
	}
	
	mutex->taken = TRUE;
	sigprocmask(SIG_UNBLOCK, &block, NULL);
	
	return 0;
}

int green_mutex_unlock(green_mutex_t *mutex) {
	sigprocmask(SIG_BLOCK, &block, NULL);
	
	// Move only one thread, as the only way its one of the suspended onces is it's trying to lock mutex
	if (mutex->suspended != NULL) {
		push_ready_queue(mutex->suspended);
		mutex->suspended = mutex->suspended->next;
	}
	
	mutex->taken = FALSE;
	sigprocmask(SIG_UNBLOCK, &block, NULL);
	
	return 0;
}
