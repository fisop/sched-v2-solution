// Part 3: custom aging test.
//
// Two CPU-bound processes compete for the CPU: one at max priority and one at
// min priority. Neither yields the CPU voluntarily, so the only mechanism
// that can take it away from them is timer preemption.
//
// With a pure priority scheduler, the min-priority process would never be
// picked while the max-priority one stays ready: that's starvation. With
// aging, its effective priority grows while it waits until it ties with the
// other one, and a tie goes to whoever is not currently running (see
// sched_yield). So the expectation is to see "low" lines interleaved before
// "high" finishes.
//
// Sizing the test is the delicate part, and the constant that governs it is
// NROUNDS, not BUSY_ITERATIONS. Let K = ENV_PRIORITY_MAX * AGING_THRESHOLD =
// 15 * 4 = 60, the worst-case wait of the aging function: the number of
// decisions "low" needs to saturate its bonus and only then be able to tie
// with "high" (below saturation its effective priority stays strictly lower,
// so it cannot win even once). Neither worker ever makes a syscall, so every
// scheduling decision comes from a timer tick and each turn lasts exactly one
// tick. That gives a CPU ratio of K to 1, and two conditions:
//
//   1. "high" has to stay alive for more than K decisions, or "low" never gets
//      a single turn. Its lifetime is NROUNDS * r decisions, where r is the
//      ticks one round takes.
//   2. "low" only accumulates 1/K of the CPU, so before "high" finishes it
//      completes (NROUNDS * r) / K / r = NROUNDS / K rounds — and therefore
//      prints that many lines. The r cancels out: raising BUSY_ITERATIONS
//      lengthens the rounds of both processes by the same factor and does not
//      move this number at all. NROUNDS has to exceed K, period.
//
// That second condition is what the first version of this test got wrong: with
// NROUNDS=20 and K=120 (the threshold used to be 8), "low" completed 1/6 of a
// round and printed nothing, and no amount of extra work per round could have
// fixed it. With NROUNDS=200 and K=60 it prints ~3 lines before "high" is done,
// and condition 1 holds with margin: measured at 10_000_000 iterations a round
// takes ~2.5 ticks, so at 3_000_000 it takes ~0.74, putting "high"'s lifetime
// at ~148 decisions against the 60 needed.
//
// To check both conditions on another host, look at the final statistics: the
// total ticks should be a few times 60, and "low" should report a maximum aging
// bonus of 15 (its bonus saturated, which is the only way it could have won).

#include <inc/lib.h>

#define NROUNDS 200
#define BUSY_ITERATIONS 3000000

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
