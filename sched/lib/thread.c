// User-space thread library (Part 4)
//
// Provides thread_create() as an abstraction over sys_thread_create().
// The caller is responsible for allocating and freeing the thread's stack.

#include <inc/lib.h>

// Size of each thread's stack (in bytes).
#define THREAD_STACK_SIZE PGSIZE
#define THREAD_STACK_PAGES (THREAD_STACK_SIZE / PGSIZE)

// Location of the thread stacks.
//
// The main thread's stack occupies [USTACKTOP - PGSIZE, USTACKTOP), so the
// threads' stacks are laid out below it, in descending order. An unmapped
// page is left between one and the next as a guard page: if a thread runs
// past the end of its stack, instead of silently writing over its
// neighbor's stack it gets a page fault.
//
// With this layout, thread n's stack lives at
//   [THREAD_STACK_BASE(n), THREAD_STACK_BASE(n) + THREAD_STACK_SIZE)
// and grows downward from THREAD_STACK_TOP(n).
#define THREAD_STACK_STRIDE (THREAD_STACK_SIZE + PGSIZE)
#define THREAD_STACK_TOP(n) (USTACKTOP - 2 * PGSIZE - (n) *THREAD_STACK_STRIDE)
#define THREAD_STACK_BASE(n) (THREAD_STACK_TOP(n) - THREAD_STACK_SIZE)

// Number of threads created so far, used as the index of the next free
// stack.
//
// It's a process-wide global variable, so every thread shares it: two
// threads calling thread_create() at the same time could read the same
// value and request the same stack. Serializing this would need
// synchronization, which is out of scope for this part.
static int nthreads;

// Creates a new thread that runs func(arg).
//
// Returns the thread's envid on success, or < 0 on error.
envid_t
thread_create(void (*func)(void *), void *arg)
{
	uintptr_t base = THREAD_STACK_BASE(nthreads);
	uintptr_t top = THREAD_STACK_TOP(nthreads);
	uint32_t *sp;
	envid_t tid;
	int i, r;

	// Allocate the stack in the current process's address space
	// (envid 0). The thread will share it, since it shares the same
	// page directory.
	for (i = 0; i < THREAD_STACK_PAGES; i++) {
		r = sys_page_alloc(0,
		                   (void *) (base + i * PGSIZE),
		                   PTE_P | PTE_U | PTE_W);
		if (r < 0) {
			while (--i >= 0)
				sys_page_unmap(0, (void *) (base + i * PGSIZE));
			return r;
		}
	}

	// Set up the stack the way the x86 calling convention expects: on
	// entering a function, esp points to the return address and esp + 4
	// to the first argument. Putting exit() as the return address makes
	// the thread terminate on its own once func returns.
	sp = (uint32_t *) top;
	*(--sp) = (uint32_t) (uintptr_t) arg;
	*(--sp) = (uint32_t) (uintptr_t) exit;

	tid = sys_thread_create((void *) func, sp);
	if (tid < 0) {
		for (i = 0; i < THREAD_STACK_PAGES; i++)
			sys_page_unmap(0, (void *) (base + i * PGSIZE));
		return tid;
	}

	nthreads++;

	return tid;
}
