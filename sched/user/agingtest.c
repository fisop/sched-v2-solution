// Part 3: custom aging test.
//
// Two CPU-bound processes compete for the CPU: one at max priority ("high")
// and one PRIO_GAP points below it ("low"). Neither yields the CPU
// voluntarily, so the only mechanism that can take it away from them is timer
// preemption.
//
// With a pure priority scheduler, the lower-priority process would never be
// picked while the higher one stays ready — any strictly lower priority
// starves, not just the minimum: that's the point. With aging, its effective
// priority grows while it waits until it ties with the other one, and a tie
// goes to whoever is not currently running (see sched_yield). So the
// expectation is to see "low" lines interleaved before "high" finishes.
//
// Sizing the test has two independent knobs, and they control different
// things:
//
//   - PRIO_GAP (the priority *distance* between "high" and "low") controls how
//     OFTEN "low" gets picked. What "low"'s bonus has to overcome is not the
//     full ENV_PRIORITY_MIN..ENV_PRIORITY_MAX range, only the gap to whatever
//     "high" actually holds: since "high" never waits (it's always either
//     running or the only other candidate), its effective priority stays
//     pinned at its base the whole time. So in this test "low" waits
//     K = PRIO_GAP * AGING_THRESHOLD = 16 decisions per turn.
//
//     Careful not to confuse that K with the guarantee of the mechanism, which
//     is a different number: the worst case the scheduler has to bound is the
//     largest gap the priority range allows, ENV_PRIORITY_MAX *
//     AGING_THRESHOLD = 60 decisions. That is what the informe reports as the
//     anti-starvation bound. This test deliberately picks a narrower gap: with
//     the extremes (PRIO_GAP = 15, K = 60) the interleaving is real but so
//     sparse that it barely shows, and a process at PRIO_GAP = 4 below the
//     maximum starves just as completely under a pure-priority policy, which
//     is what the demonstration needs.
//
//   - NROUNDS controls how long the test runs for a fixed ratio, i.e. how many
//     lines get printed. Neither worker ever makes a syscall, so every
//     scheduling decision comes from a timer tick and each turn lasts exactly
//     one tick, giving a CPU ratio of K to 1. Two conditions:
//
//       1. "high" has to stay alive for more than K decisions, or "low" never
//          gets a single turn. Its lifetime is NROUNDS * r decisions, where r
//          is the ticks one round takes.
//       2. "low" only accumulates 1/K of the CPU, so before "high" finishes it
//          completes (NROUNDS * r) / K / r = NROUNDS / K rounds — and
//          therefore prints that many lines. The r cancels out: raising
//          BUSY_ITERATIONS lengthens the rounds of both processes by the same
//          factor and moves neither this count nor the K-to-1 ratio at all —
//          it only changes wall-clock time.
//
// Measured on a real run with these constants: a round takes r ~= 0.72 ticks,
// so "high" lives ~108 decisions against the 16 that condition 1 needs, and
// "low" wins one turn every 16-17 ticks, printing NROUNDS / K ~= 9 lines
// spread through the run.
//
// To check both conditions on another host, look at the final statistics: the
// total ticks should be several times K, and "low" should report a maximum
// aging bonus of exactly PRIO_GAP (its bonus saturated at the gap, which is
// the only way it could ever have won a tie).

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
worker(const char *name, int inherited)
{
	int i;

	// fork() leaves this child ENV_RUNNABLE with the priority it inherited
	// and the parent only assigns the real one once fork() has returned.
	// Running rounds at the inherited priority would mix a different gap
	// into the same measurement, so wait for the assignment first. This is
	// cheap: while waiting, the child is tied with the parent, and a tie
	// goes to whoever is not currently running, so each yield hands the CPU
	// straight back to the parent.
	while (sys_getpriority(0) == inherited)
		sys_yield();

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
	int base = sys_getpriority(0);
	int hi_prio = ENV_PRIORITY_MAX;
	int lo_prio = ENV_PRIORITY_MAX - PRIO_GAP;

	// PRIO_GAP is what actually sets the win frequency (see the header
	// comment), not the absolute values, so this assert is the only thing
	// standing between a valid test and lo_prio silently going out of
	// range if PRIO_GAP is ever changed without checking ENV_PRIORITY_MIN.
	static_assert(ENV_PRIORITY_MAX - PRIO_GAP >= ENV_PRIORITY_MIN);

	cprintf("agingtest: prioridad por defecto = %d, rango [%d, %d], "
	        "brecha usada = %d\n",
	        base,
	        ENV_PRIORITY_MIN,
	        ENV_PRIORITY_MAX,
	        PRIO_GAP);

	// Both children are forked before either priority is assigned. Doing it
	// the other way around —assign "high" and only then fork "low"— starves
	// the parent for AGING_THRESHOLD * (hi_prio - base) = 28 decisions right
	// in the middle of the setup, because "high" already outranks it. That
	// delay was a quarter of "high"'s whole lifetime, and "low" spent it at
	// the inherited priority, under a gap that isn't the one being measured.
	hi = fork();
	if (hi < 0)
		panic("fork: %e", hi);
	if (hi == 0) {
		worker("ALTA", base);
		exit();
	}

	lo = fork();
	if (lo < 0)
		panic("fork: %e", lo);
	if (lo == 0) {
		worker("BAJA", base);
		exit();
	}

	cprintf("agingtest: [%08x] -> prioridad %d, [%08x] -> prioridad %d\n",
	        hi,
	        hi_prio,
	        lo,
	        lo_prio);

	// "low" first, "high" last: the parent still outranks nobody once
	// "high" is set, so whatever is left after that assignment runs at the
	// mercy of aging. Leaving the smaller jump for last keeps the worst case
	// at 12 decisions instead of 28, and the barrier in worker() makes the
	// delay harmless either way.
	if ((r = sys_setpriority(lo, lo_prio)) < 0)
		panic("setpriority(baja): %e", r);
	if ((r = sys_setpriority(hi, hi_prio)) < 0)
		panic("setpriority(alta): %e", r);

	// The parent waits by sleeping, not by yielding: that way it drops out
	// of the ready queue and doesn't compete for CPU with the two workers.
	while (alive(hi) || alive(lo))
		sys_sleep(10);

	cprintf("agingtest: fin\n");
}