// Part 3: custom aging test.
//
// Two CPU-bound processes compete for the CPU: one at max priority and one at
// min priority. Neither yields the CPU voluntarily, so the only mechanism
// that can take it away from them is timer preemption.
//
// With a pure priority scheduler, the min-priority process would never be
// picked while the max-priority one stays ready: that's starvation. With
// aging, its effective priority grows while it waits until it catches up to
// the other one, and per the scheduler's design (sched_effective_priority)
// ties are resolved in favor of whoever has gone the longest without
// running. So the expectation is to see "low" lines interleaved before
// "high" finishes.
//
// The constants are chosen so that the high-priority process stays CPU-bound
// well past ENV_PRIORITY_MAX * AGING_THRESHOLD = 120 ticks (the number the
// other one needs to saturate its aging bonus and only then be able to beat
// it: below saturation, its effective priority stays strictly lower than
// "high"'s, so it can't win even once). Measured on a real run, NROUNDS=20
// with BUSY_ITERATIONS=10_000_000 only produced ~47 ticks total — far below
// the threshold, so "low" never got to run while "high" was still alive. If
// "low" still doesn't show up until "high" finishes completely with the
// current value, raise BUSY_ITERATIONS (or lower AGING_THRESHOLD in
// kern/sched.c) until "timer ticks since boot" in the final statistics comes
// out well above 120 for the whole run.

#include <inc/lib.h>

#define NROUNDS 20
#define BUSY_ITERATIONS 100000000

// An environment is still alive as long as its slot in envs[] (mapped
// read-only at UENVS) is still its own and hasn't gone back to the free list.
static bool
alive(envid_t id)
{
	const volatile struct Env *e = &envs[ENVX(id)];

	return e->env_id == id && e->env_status != ENV_FREE;
}

static void
worker(const char *name)
{
	int i;

	for (i = 0; i < NROUNDS; i++) {
		volatile int j;

		// Pure CPU work: no syscalls, no sys_yield.
		for (j = 0; j < BUSY_ITERATIONS; j++)
			;

		cprintf("[%08x] %s (prioridad %d): ronda %d\n",
		        sys_getenvid(),
		        name,
		        sys_getpriority(0),
		        i);
	}

	cprintf("[%08x] %s: termine\n", sys_getenvid(), name);
}

void
umain(int argc, char **argv)
{
	envid_t hi, lo;
	int r;

	cprintf("agingtest: prioridad por defecto = %d, rango [%d, %d]\n",
	        sys_getpriority(0),
	        ENV_PRIORITY_MIN,
	        ENV_PRIORITY_MAX);

	hi = fork();
	if (hi < 0)
		panic("fork: %e", hi);
	if (hi == 0) {
		worker("alta");
		exit();
	}
	if ((r = sys_setpriority(hi, ENV_PRIORITY_MAX)) < 0)
		panic("setpriority(alta): %e", r);

	lo = fork();
	if (lo < 0)
		panic("fork: %e", lo);
	if (lo == 0) {
		worker("baja");
		exit();
	}
	if ((r = sys_setpriority(lo, ENV_PRIORITY_MIN)) < 0)
		panic("setpriority(baja): %e", r);

	cprintf("agingtest: [%08x] prioridad %d, [%08x] prioridad %d\n",
	        hi,
	        ENV_PRIORITY_MAX,
	        lo,
	        ENV_PRIORITY_MIN);

	// The parent waits by sleeping, not by yielding: that way it drops out
	// of the ready queue and doesn't compete for CPU with the two workers.
	while (alive(hi) || alive(lo))
		sys_sleep(10);

	cprintf("agingtest: fin\n");
}
