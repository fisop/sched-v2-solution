#include <inc/assert.h>
#include <inc/x86.h>
#include <kern/spinlock.h>
#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/monitor.h>
#include <kern/trap.h>

void sched_halt(void);

// Every AGING_THRESHOLD scheduling decisions spent waiting, an environment
// gains one point of effective priority. See sched_effective_priority().
//
// The value is squeezed from both sides. Upwards, because the worst case wait
// is ENV_PRIORITY_MAX * AGING_THRESHOLD decisions (what an environment at the
// minimum priority needs to catch up with one at the maximum): with 8 that's
// 120 decisions, more than a short run produces, and the anti-starvation
// guarantee becomes unobservable. Downwards, because user/priotest.c compares
// children one priority point apart, and a threshold of 1 or 2 lets aging
// reorder them so fast that the priority itself stops being visible. 4 leaves
// the worst case at 60 decisions and still needs 4 consecutive decisions to
// overcome a single point of priority.
#define AGING_THRESHOLD 4

// Number of scheduling decisions remembered in the history.
#define SCHED_HISTORY_LEN 16

#ifdef SCHED_PRIORITIES
static uint32_t sched_effective_priority(struct Env *e);

// Effective priority of an environment under the active policy. Round robin
// ignores priorities altogether, so there the effective one is just the base.
#define EFFECTIVE_PRIORITY(e) sched_effective_priority(e)
#else
#define EFFECTIVE_PRIORITY(e) ((e)->env_priority)
#endif


/****************************************************************
 * Scheduler statistics (Parts 2 and 3)
 ****************************************************************/

// Calls to sched_yield() since the kernel booted.
static uint32_t sched_calls;

// One entry per slot of the envs[] array. env_id lets us detect that the slot
// was recycled by a new environment and reset its counters to zero.
struct EnvStat {
	envid_t env_id;
	uint32_t runs;      // times the scheduler picked it
	uint32_t cputicks;  // CPU ticks it accumulated
	uint32_t priority;  // last priority it ran with
	uint32_t maxbonus;  // largest aging bonus it ever got picked with
};
static struct EnvStat sched_env_stats[NENV];

// Circular history of the most recent scheduling decisions.
struct SchedHistory {
	envid_t env_id;
	uint32_t tick;       // tick at which it started running
	uint32_t priority;   // base priority it was picked with
	uint32_t effective;  // effective priority (base + aging bonus)
};
static struct SchedHistory sched_history[SCHED_HISTORY_LEN];
static uint32_t sched_history_count;

// CPU ticks accumulated per priority level.
static uint32_t sched_prio_ticks[NPRIORITIES];

// Environment currently occupying the CPU, so its ticks can be attributed on
// the next scheduling decision.
static envid_t sched_running_id;
static uint32_t sched_running_prio;
static uint32_t sched_running_since;

// Attributes to the environment that was running the ticks it consumed since
// the last scheduling decision.
//
// With more than one CPU the attribution is approximate: sched_yield() runs
// per CPU, but 'ticks' and these counters are global, so two environments
// running in parallel split the same wall-clock ticks between them.
static void
sched_account_cputicks(void)
{
	uint32_t delta;

	if (sched_running_id == 0)
		return;

	delta = ticks - sched_running_since;
	sched_env_stats[ENVX(sched_running_id)].cputicks += delta;
	if (sched_running_prio < NPRIORITIES)
		sched_prio_ticks[sched_running_prio] += delta;

	sched_running_id = 0;
}

// Records the scheduling decision and runs the chosen environment.
// Does not return.
//
// 'effective' is the effective priority the environment was picked with. It
// has to come from the caller because the priority policy resets the winner's
// wait counter before running it, so by this point the bonus can no longer be
// recomputed.
static void sched_run(struct Env *e, uint32_t effective) __attribute__((noreturn));

static void
sched_run(struct Env *e, uint32_t effective)
{
	struct EnvStat *st = &sched_env_stats[ENVX(e->env_id)];
	struct SchedHistory *h;
	uint32_t bonus = effective - e->env_priority;

	sched_account_cputicks();

	// The slot ENVX(e->env_id) may have previously belonged to another
	// environment that already finished (slots get recycled). If we don't
	// flush its data before overwriting it, its statistics are lost
	// without a trace in sched_print_stats' final table.
	if (st->env_id != e->env_id) {
		if (st->env_id != 0 && st->runs > 0)
			cprintf("  [%08x] (finalizado, slot reciclado) elegido "
			        "%d veces, %d ticks de CPU, prioridad %d\n",
			        st->env_id,
			        st->runs,
			        st->cputicks,
			        st->priority);
		st->env_id = e->env_id;
		st->runs = 0;
		st->cputicks = 0;
		st->maxbonus = 0;
	}
	st->runs++;
	st->priority = e->env_priority;
	if (bonus > st->maxbonus)
		st->maxbonus = bonus;

	h = &sched_history[sched_history_count % SCHED_HISTORY_LEN];
	h->env_id = e->env_id;
	h->tick = ticks;
	h->priority = e->env_priority;
	h->effective = effective;
	sched_history_count++;

	sched_running_id = e->env_id;
	sched_running_prio = e->env_priority;
	sched_running_since = ticks;

	env_run(e);
}

static void
sched_print_stats(void)
{
	uint32_t i, n, total;

	sched_account_cputicks();

	cprintf("\n=== estadisticas del scheduler ===\n");
#ifdef SCHED_ROUND_ROBIN
	cprintf("politica: round robin preemptivo\n");
#endif
#ifdef SCHED_PRIORITIES
	cprintf("politica: prioridades con aging (umbral: %d decisiones de "
	        "espera por punto, peor caso %d)\n",
	        AGING_THRESHOLD,
	        ENV_PRIORITY_MAX * AGING_THRESHOLD);
#endif
	cprintf("llamadas al scheduler (sched_yield): %d\n", sched_calls);
	cprintf("ticks de timer desde el arranque: %d\n", ticks);

	cprintf("\nejecuciones por environment:\n");
	for (i = 0; i < NENV; i++) {
		if (sched_env_stats[i].env_id == 0)
			continue;
		cprintf("  [%08x] elegido %d veces, %d ticks de CPU, prioridad "
		        "%d",
		        sched_env_stats[i].env_id,
		        sched_env_stats[i].runs,
		        sched_env_stats[i].cputicks,
		        sched_env_stats[i].priority);
#ifdef SCHED_PRIORITIES
		// The base priority never changes with aging: the bonus lives
		// in the effective priority, which is recomputed on every
		// decision. Without printing it, a run where aging was decisive
		// looks identical to one where it never fired.
		cprintf(" (bonus maximo de aging: %d)",
		        sched_env_stats[i].maxbonus);
#endif
		cprintf("\n");
	}

	n = sched_history_count < SCHED_HISTORY_LEN ? sched_history_count
	                                            : SCHED_HISTORY_LEN;
	cprintf("\nultimas %d decisiones de scheduling (de la mas reciente a "
	        "la "
	        "mas vieja):\n",
	        n);
	for (i = 0; i < n; i++) {
		struct SchedHistory *h =
		        &sched_history[(sched_history_count - 1 - i) % SCHED_HISTORY_LEN];
#ifdef SCHED_PRIORITIES
		cprintf("  [%08x] desde el tick %d (prioridad %d, efectiva "
		        "%d)\n",
		        h->env_id,
		        h->tick,
		        h->priority,
		        h->effective);
#else
		cprintf("  [%08x] desde el tick %d (prioridad %d)\n",
		        h->env_id,
		        h->tick,
		        h->priority);
#endif
	}

	total = 0;
	for (i = 0; i < NPRIORITIES; i++)
		total += sched_prio_ticks[i];

	cprintf("\ndistribucion de CPU por prioridad (%d ticks atribuidos):\n",
	        total);
	for (i = 0; i < NPRIORITIES; i++) {
		if (sched_prio_ticks[i] == 0)
			continue;
		cprintf("  prioridad %2d: %d ticks (%d%%)\n",
		        i,
		        sched_prio_ticks[i],
		        total ? sched_prio_ticks[i] * 100 / total : 0);
	}
	cprintf("\n");
}


/****************************************************************
 * Sleep (Part 2)
 ****************************************************************/

// Wakes up environments that were sleeping and whose env_sleep_until <= ticks.
// The global counter 'ticks' is declared in kern/trap.h.
// Call from trap_dispatch after incrementing ticks and before sched_yield.
void
sched_wakeup_sleeping(void)
{
	size_t i;

	for (i = 0; i < NENV; i++) {
		struct Env *e = &envs[i];

		// env_sleep_until == 0 distinguishes environments that are
		// ENV_NOT_RUNNABLE for another reason (for example, blocked
		// waiting on an IPC), which should not be woken up.
		if (e->env_status != ENV_NOT_RUNNABLE || e->env_sleep_until == 0)
			continue;

		if (e->env_sleep_until <= ticks) {
			e->env_sleep_until = 0;
			e->env_status = ENV_RUNNABLE;
		}
	}
}


/****************************************************************
 * Scheduling policy
 ****************************************************************/

#ifdef SCHED_PRIORITIES
// Effective priority: the base priority plus a bonus that grows with how
// long the environment has been waiting in the ready queue.
//
// The bonus saturates at ENV_PRIORITY_MAX, which is enough for any postponed
// environment to catch up to the system's maximum priority: once it ties, it
// wins, because the environment that is currently running only keeps the CPU
// with a strictly higher effective priority (see sched_yield). Saturating it
// keeps the effective priority bounded and makes the worst-case wait
// predictable: ENV_PRIORITY_MAX * AGING_THRESHOLD scheduling decisions.
static uint32_t
sched_effective_priority(struct Env *e)
{
	uint32_t bonus = e->env_wait_ticks / AGING_THRESHOLD;

	if (bonus > ENV_PRIORITY_MAX)
		bonus = ENV_PRIORITY_MAX;

	return e->env_priority + bonus;
}
#endif

// Choose a user environment to run and run it.
void
sched_yield(void)
{
	size_t i, start;

	sched_calls++;

	// The scan starts right after the environment that was running on
	// this CPU, so as not to always pick it again.
	start = curenv ? ENVX(curenv->env_id) + 1 : 0;

#ifdef SCHED_ROUND_ROBIN
	// First ENV_RUNNABLE in circular order. An environment running on
	// another CPU has env_status == ENV_RUNNING, so this filter already
	// excludes it.
	for (i = 0; i < NENV; i++) {
		struct Env *e = &envs[(start + i) % NENV];

		if (e->env_status == ENV_RUNNABLE)
			sched_run(e, e->env_priority);
	}
#endif

#ifdef SCHED_PRIORITIES
	struct Env *best = NULL;
	uint32_t best_prio = 0;

	// Highest effective priority among the ENV_RUNNABLE ones. On a tie the
	// first one in the circular scan wins. Note that this orders ties by
	// slot index (starting right after the current environment), not by
	// how long each one has been waiting: with several environments tied
	// at the same effective priority, the scan does not favor the oldest.
	// What makes aging an anti-starvation mechanism is not this tie-break
	// but the one against curenv, further down.
	for (i = 0; i < NENV; i++) {
		struct Env *e = &envs[(start + i) % NENV];
		uint32_t prio;

		if (e->env_status != ENV_RUNNABLE)
			continue;

		prio = sched_effective_priority(e);
		if (best == NULL || prio > best_prio) {
			best = e;
			best_prio = prio;
		}
	}

	// The environment that is running right now (curenv) doesn't show up
	// in the loop above: its env_status is still ENV_RUNNING, not
	// ENV_RUNNABLE (it only becomes RUNNABLE inside env_run, once another
	// one has already been picked). Without this step, with only two
	// CPU-bound processes competing the scheduler would alternate between
	// them one by one on every tick regardless of priority, because the
	// only candidate the loop above can ever find is always "the other
	// one".
	//
	// curenv only wins with a STRICTLY higher priority (never on a tie):
	// if another environment's aging bonus saturates at ENV_PRIORITY_MAX
	// and ties with curenv, the one that has been waiting must win, not
	// the one that's already running — otherwise that perpetual tie would
	// bring back the starvation that aging is meant to prevent.
	if (curenv && curenv->env_status == ENV_RUNNING) {
		uint32_t curprio = sched_effective_priority(curenv);

		if (best == NULL || curprio > best_prio) {
			best = curenv;
			best_prio = curprio;
		}
	}

	if (best != NULL) {
		// Aging: the chosen one goes back to zero, everyone else in
		// the ready queue accrues wait time. If the chosen one is
		// curenv, this is a no-op (its wait_ticks is already 0 from
		// the last time it was picked).
		for (i = 0; i < NENV; i++) {
			struct Env *e = &envs[i];

			if (e != best && e->env_status == ENV_RUNNABLE)
				e->env_wait_ticks++;
		}
		best->env_wait_ticks = 0;

		sched_run(best, best_prio);
	}
#endif

	// No environment is ready. If the one on this CPU is still
	// ENV_RUNNING (for example, it was preempted by the timer and is the
	// only process alive), it can be picked again.
	if (curenv && curenv->env_status == ENV_RUNNING)
		sched_run(curenv, EFFECTIVE_PRIORITY(curenv));

	// sched_halt never returns
	sched_halt();
}

// Halt this CPU when there is nothing to do. Wait until the
// timer interrupt wakes it up. This function never returns.
//
void
sched_halt(void)
{
	int i;
	bool pending = false;

	// For debugging and testing purposes, if there are no runnable
	// environments in the system, then drop into the kernel monitor.
	//
	// An environment asleep via sys_sleep is ENV_NOT_RUNNABLE, so it's not
	// enough to just look for ready environments: if everyone is asleep
	// at once we still need to fall through to the hlt below, because the
	// timer will wake them up. Only once no environment is left alive or
	// asleep has the work truly finished, and that's when statistics
	// should be printed.
	for (i = 0; i < NENV; i++) {
		if (envs[i].env_status == ENV_RUNNABLE ||
		    envs[i].env_status == ENV_RUNNING ||
		    envs[i].env_status == ENV_DYING ||
		    (envs[i].env_status == ENV_NOT_RUNNABLE &&
		     envs[i].env_sleep_until != 0)) {
			pending = true;
			break;
		}
	}
	if (!pending) {
		cprintf("No runnable environments in the system!\n");

		// Once the scheduler has finishied it's work, print statistics
		// on performance.
		sched_print_stats();

		while (1)
			monitor(NULL);
	}

	// Mark that no environment is running on this CPU
	curenv = NULL;
	lcr3(PADDR(kern_pgdir));

	// Mark that this CPU is in the HALT state, so that when
	// timer interupts come in, we know we should re-acquire the
	// big kernel lock
	xchg(&thiscpu->cpu_status, CPU_HALTED);

	// Release the big kernel lock as if we were "leaving" the kernel
	unlock_kernel();

	// Reset stack pointer, enable interrupts and then halt.
	asm volatile("movl $0, %%ebp\n"
	             "movl %0, %%esp\n"
	             "pushl $0\n"
	             "pushl $0\n"
	             "sti\n"
	             "1:\n"
	             "hlt\n"
	             "jmp 1b\n"
	             :
	             : "a"(thiscpu->cpu_ts.ts_esp0));
}
