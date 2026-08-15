// Part 4: test for user-space threads.
//
// First phase: NTHREADS threads increment a process-wide global variable.
// Since they share the page directory, they all see each other's changes and
// the final total is exactly NTHREADS * NITER. With fork(), where each child
// takes its own copy of memory, the total would end up at 0.
//
// Second phase: the same variable is incremented without synchronization,
// reading and writing with a yield in between, to expose the race condition.

#include <inc/lib.h>

#define NTHREADS 3
#define NITER 5

// Variables shared by every thread in the process (they live in the BSS,
// which is part of the shared address space).
static volatile int shared_counter = 0;
static volatile int race_counter = 0;

// Function each thread runs.
//
// It uses sys_getenvid() to identify itself: thisenv is a process-wide
// global variable, shared among all the threads, and points to the main
// process's env.
static void
thread_func(void *arg)
{
	int id = (int) (uintptr_t) arg;
	int i;

	for (i = 0; i < NITER; i++) {
		cprintf("thread %d [%08x]: shared_counter %d -> %d\n",
		        id,
		        sys_getenvid(),
		        shared_counter,
		        shared_counter + 1);
		shared_counter++;
		sys_yield();
	}

	// Read-modify-write without synchronization: the CPU is yielded on
	// purpose between the read and the write, so the lost update is always
	// visible and doesn't depend on where the timer interrupt happens to
	// land.
	for (i = 0; i < NITER; i++) {
		int tmp = race_counter;
		sys_yield();
		race_counter = tmp + 1;
	}

	cprintf("thread %d [%08x]: fin\n", id, sys_getenvid());
}

// Waits for the threads to finish. The envs array is mapped read-only in
// user space (UENVS), so it's enough to check each one's status: once its
// slot goes ENV_FREE or gets reused by another environment, the thread has
// finished.
static void
thread_join_all(envid_t *tids, int n)
{
	int i, alive;

	do {
		alive = 0;
		for (i = 0; i < n; i++) {
			const volatile struct Env *e = &envs[ENVX(tids[i])];

			if (e->env_id == tids[i] && e->env_status != ENV_FREE)
				alive++;
		}
		if (alive)
			sys_yield();
	} while (alive);
}

void
umain(int argc, char **argv)
{
	envid_t tids[NTHREADS];
	int i;

	cprintf("main [%08x]: creando %d threads\n", sys_getenvid(), NTHREADS);

	for (i = 0; i < NTHREADS; i++) {
		tids[i] = thread_create(thread_func, (void *) (uintptr_t) i);
		if (tids[i] < 0)
			panic("thread_create: %e", tids[i]);
		cprintf("main: thread %d -> [%08x]\n", i, tids[i]);
	}

	thread_join_all(tids, NTHREADS);

	cprintf("main: shared_counter = %d (esperado %d)\n",
	        shared_counter,
	        NTHREADS * NITER);
	cprintf("main: race_counter = %d (esperado %d si no hubiera condicion "
	        "de carrera)\n",
	        race_counter,
	        NTHREADS * NITER);
}
