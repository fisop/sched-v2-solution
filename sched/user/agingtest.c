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
// Sizing the test has two independent knobs, and they control different
// things:
//
//   - PRIO_GAP (the priority *distance* between "high" and "low") controls
//     how OFTEN "low" gets picked. What "low"'s bonus has to overcome is not
//     the full ENV_PRIORITY_MIN..ENV_PRIORITY_MAX range, only the gap to
//     whatever "high" actually holds: since "high" never waits (it's always
//     either running or the only other candidate), its effective priority
//     stays pinned at its base the whole time. So the real worst-case wait is
//     K = PRIO_GAP * AGING_THRESHOLD decisions, not
//     ENV_PRIORITY_MAX * AGING_THRESHOLD. Using the full MIN/MAX extremes
//     (PRIO_GAP = 15) is what produced the 1-in-60 ratio from the earlier
//     version of this test: correct, but so infrequent that the interleaving
//     was barely visible. Shrinking the gap to 4 drops that to K = 16,
//     roughly 4x more frequent, while still starving "low" completely under
//     a pure-priority policy — which is exactly what this test needs to show.
//
//   - NROUNDS controls how many total lines get printed, i.e. how long the
//     test runs for a fixed ratio. Neither worker ever makes a syscall, so
//     every scheduling decision comes from a timer tick and each turn lasts
//     exactly one tick, giving a CPU ratio of K to 1. Two conditions:
//
//       1. "high" has to stay alive for more than K decisions, or "low"
//          never gets a single turn. Its lifetime is NROUNDS * r decisions,
//          where r is the ticks one round takes.
//       2. "low" only accumulates 1/K of the CPU, so before "high" finishes
//          it completes (NROUNDS * r) / K / r = NROUNDS / K rounds — and
//          therefore prints that many lines. The r cancels out: raising
//          BUSY_ITERATIONS lengthens the rounds of both processes by the same
//          factor and moves neither this count nor the K-to-1 ratio at all —
//          it only changes wall-clock time, so it is not a knob for
//          frequency or line count, just for how fast the test runs.
//
// With PRIO_GAP=4 (K=16) and NROUNDS=200, "low" should print NROUNDS/K = 12
// lines fairly evenly spread through "high"'s run, instead of 1 every 60
// decisions. Condition 1 still holds with comfortable margin: measured at
// 10_000_000 iterations a round takes ~2.5 ticks, so at 3_000_000 it takes
// ~0.74, putting "high"'s lifetime at ~148 decisions against the 16 needed.
//
// To check both on another host, look at the final statistics: the total
// ticks should be a few times 16 (not 60), and "low" should report a maximum
// aging bonus of PRIO_GAP = 4 (its bonus saturated at the gap, which is the
// only way it could have won a tie).

#include <inc/lib.h>

#define NROUNDS 150
#define BUSY_ITERATIONS 3000000
#define PRIO_GAP 4

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

		// Pure CPU work: no syscalls, no sys_yield. This is what
		// forces every scheduling decision here to come from timer
		// preemption, which is what the sizing analysis above assumes.
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
	int hi_prio = ENV_PRIORITY_MAX;
	int lo_prio = ENV_PRIORITY_MAX - PRIO_GAP;

	// PRIO_GAP is what actually sets the win frequency (see the header
	// comment), not the absolute values, so this assert is the only thing
	// standing between a valid test and lo_prio silently going out of
	// range if PRIO_GAP is ever changed without checking ENV_PRIORITY_MIN.
	static_assert(ENV_PRIORITY_MAX - PRIO_GAP >= ENV_PRIORITY_MIN);

	cprintf("agingtest: prioridad por defecto = %d, rango [%d, %d], "
	        "brecha usada = %d\n",
	        sys_getpriority(0),
	        ENV_PRIORITY_MIN,
	        ENV_PRIORITY_MAX,
	        PRIO_GAP);

	hi = fork();
	if (hi < 0)
		panic("fork: %e", hi);
	if (hi == 0) {
		worker("ALTA");
		exit();
	}
	if ((r = sys_setpriority(hi, hi_prio)) < 0)
		panic("setpriority(alta): %e", r);

	lo = fork();
	if (lo < 0)
		panic("fork: %e", lo);
	if (lo == 0) {
		worker("BAJA");
		exit();
	}
	if ((r = sys_setpriority(lo, lo_prio)) < 0)
		panic("setpriority(baja): %e", r);

	cprintf("agingtest: [%08x] prioridad %d, [%08x] prioridad %d\n",
	        hi,
	        hi_prio,
	        lo,
	        lo_prio);

	// The parent waits by sleeping, not by yielding: that way it drops out
	// of the ready queue and doesn't compete for CPU with the two workers.
	while (alive(hi) || alive(lo))
		sys_sleep(10);

	cprintf("agingtest: fin\n");
}