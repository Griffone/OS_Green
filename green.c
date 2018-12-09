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
//
// NO PROMISES ON CORRECT FUNCTIONALITY IF SET TO 0!
// That said, since assert would panic the program on triggering if your tests run fine, you can disable it
#define CLEAN_NEXT			1	// Make sure the green_t.next is NULL on a freshly popped thread
#define NON_EMPTY_ASSERT	1	// Check queue emptiness on popping


static ucontext_t		main_context = {0};
static green_t			main_green = {&main_context, NULL, NULL, NULL, NULL, FALSE};

static sigset_t			block;

static green_t			*running = &main_green;

static green_queue_t	ready_queue;


static inline void push_queue(green_queue_t *queue, green_t *thread) {
	queue->back = (queue->back) ? (queue->back->next = thread) : (queue->front = thread);
}

static inline green_t *pop_queue(green_queue_t *queue) {
#if NON_EMPTY_ASSERT
	assert(queue->front != NULL);
#endif // NON_EMPTY_ASSERT
	green_t *thread = queue->front;
	queue->front = thread->next;
	
	if (thread->next == NULL) queue->back = NULL;

#if CLEAN_NEXT
	thread->next = NULL;
#endif // CLEAN_NEXT

	return thread;
}

static inline void init_queue(green_queue_t *queue) {
	queue->front = queue->back = NULL;
}

static inline void block_interrupts() {
	sigprocmask(SIG_BLOCK, &block, NULL);
}

static inline void unblock_interrupts() {
	sigprocmask(SIG_UNBLOCK, &block, NULL);
}


void timer_handler(int);


static void init()	__attribute__((constructor));	// why hate on C, only any library you add can have an invisible initialize function


void init() {
	getcontext(&main_context);
	
	init_queue(&ready_queue);
	
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
	
	block_interrupts();
}

void green_thread() {
	green_t *this = running;
	
	(*this->func)(this->arg);	// execute user function
	
	// We are in key area, so make sure not to corrupt any queues
	// Place waiting thread to the ready queue
	block_interrupts();
	while (this->join != NULL) {
		push_queue(&ready_queue, this->join);
		this->join = this->join->next;
	}
	unblock_interrupts();
	
	// Free allocated memory
	free(this->context->uc_stack.ss_sp);
	free(this->context);
	
	// this thread is now a zombie
	this->zombie = TRUE;
	
	block_interrupts();
	running = pop_queue(&ready_queue);
	setcontext(running->context);
	unblock_interrupts();
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
	
	block_interrupts();
	push_queue(&ready_queue, new);
	unblock_interrupts();
	
	return 0;
}

int green_yield() {
	block_interrupts();
	green_t *suspended = running;
	
	push_queue(&ready_queue, suspended);
	
	running = pop_queue(&ready_queue);
	swapcontext(suspended->context, running->context);
	unblock_interrupts();
	
	return 0;
}

int green_join(green_t *thread) {
	if (thread->zombie) return 0;
	
	block_interrupts();
	green_t *suspended = running;
	
	// This is smart
	// Make next null if we're the only ones waiting
	// Make next something if someone is already waiting
	suspended->next = thread->join;
	thread->join = suspended;
	
	running = pop_queue(&ready_queue);
	swapcontext(suspended->context, running->context);
	unblock_interrupts();
	
	return 0;
}

void green_cond_init(green_cond_t *condition) {
	init_queue(&condition->queue);
}

int green_cond_wait(green_cond_t *condition, green_mutex_t *mutex) {
	block_interrupts();
	green_t *suspended = running;
	push_queue(&condition->queue, suspended);
	
	if (mutex != NULL) {
		// Mirror green_mutex_unlock without signal unblocking	
		if (mutex->queue.front != NULL) {
			push_queue(&ready_queue, pop_queue(&mutex->queue));
		}
		
		mutex->taken = FALSE;
	}
	
	running = pop_queue(&ready_queue);
	swapcontext(suspended->context, running->context);
	
	// Remember, we got swapped back into focus now
	if (mutex != NULL) {
		while (mutex->taken) {
			push_queue(&mutex->queue, suspended);
			
			running = pop_queue(&ready_queue);
			swapcontext(suspended->context, running->context);
		}
		
		mutex->taken = TRUE;
	}
	
	unblock_interrupts();
	
	return 0;
}

void green_cond_signal(green_cond_t *condition) {
	if (condition->queue.front == NULL) return;

	// Fairly straight forward
	block_interrupts();
	push_queue(&ready_queue, pop_queue(&condition->queue));
	unblock_interrupts();
}

void timer_handler(int sig) {
	green_t *suspended = running;
	
	push_queue(&ready_queue, suspended);
	
	running = pop_queue(&ready_queue);
	swapcontext(suspended->context, running->context);
}

void green_mutex_init(green_mutex_t *mutex) {
	mutex->taken = FALSE;
	init_queue(&mutex->queue);
}

int green_mutex_lock(green_mutex_t *mutex) {
	block_interrupts();
	
	green_t *suspended = running;
	while (mutex->taken) {
		push_queue(&mutex->queue, suspended);
		
		running = pop_queue(&ready_queue);
		swapcontext(suspended->context, running->context);
	}
	
	mutex->taken = TRUE;
	unblock_interrupts();
	
	return 0;
}

int green_mutex_unlock(green_mutex_t *mutex) {
	block_interrupts();
	
	// Move only one thread, as the only way its one of the suspended onces is it's trying to lock mutex
	if (mutex->queue.front != NULL) {
		push_queue(&ready_queue, pop_queue(&mutex->queue));
	}
	
	mutex->taken = FALSE;
	unblock_interrupts();
	
	return 0;
}
