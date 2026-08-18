// Part 3: test for the priority scheduler.
//
// The parent creates several children and assigns each one a different
// priority. This is allowed by sys_setpriority's rules: a process can modify
// the priority of its direct children to any value.
//
// The children deliberately get priorities *below* the parent's. fork() leaves
// the child ENV_RUNNABLE with the priority it inherited, and the parent only
// assigns the real one once fork() has already returned. If the children ended
// up above the parent, the first one would preempt it and run to completion
// before the parent got to create the next one, so there would never be more
// than one child competing at a time and the test would compare nothing.
// Staying above all of them lets the parent create the whole set, and then it
// releases them all at once by lowering its own priority — which is exactly
// the operation the security rules do allow.
//
// Each child prints its progress while yielding the CPU on every iteration,
// so the order and frequency of the lines show who the scheduler favors.
//
// Priorities are expressed relative to the default returned by
// sys_getpriority(), so as not to depend on the range chosen in inc/env.h. The
// test needs a default of at least NCHILD + 1 to have room for one level per
// child plus one below all of them for the parent.

#include <inc/lib.h>

#define NCHILD 4
#define NITER 5

// An environment is still alive as long as its slot in envs[] (mapped
// read-only at UENVS) is still its own and hasn't gone back to the free list.
static bool
alive(envid_t id)
{
	const volatile struct Env *e = &envs[ENVX(id)];

	return e->env_id == id && e->env_status != ENV_FREE;
}

static void
worker(int inherited)
{
	int prio, i;

	// The parent assigns the priority right after fork() returns, but by
	// then this child is already runnable and tied with it, so it may get
	// the CPU first. Wait for the assignment instead of running with the
	// inherited priority.
	while ((prio = sys_getpriority(0)) == inherited)
		sys_yield();

	for (i = 0; i < NITER; i++) {
		cprintf("[%08x] prio=%d iteracion %d\n", thisenv->env_id, prio, i);
		sys_yield();
	}
}

void
umain(int argc, char **argv)
{
	envid_t children[NCHILD];
	int base = sys_getpriority(0);
	int own, i, r;

	cprintf("priotest: prioridad por defecto = %d\n", base);

	if (base < NCHILD + 1) {
		cprintf("priotest: la prioridad por defecto (%d) no deja lugar "
		        "para %d hijos por debajo\n",
		        base,
		        NCHILD);
		return;
	}

	// One level per child, all of them below the parent, from highest to
	// lowest.
	for (i = 0; i < NCHILD; i++) {
		int prio = base - 1 - i;
		envid_t child = fork();

		if (child < 0)
			panic("priotest: fork: %e", child);

		if (child == 0) {
			worker(base);
			exit();
		}

		children[i] = child;
		r = sys_setpriority(child, prio);
		cprintf("priotest: hijo [%08x] -> prioridad %d (r=%d)\n",
		        child,
		        prio,
		        r);
	}

	// Security rule: a process cannot raise its own priority.
	r = sys_setpriority(0, base + 1);
	if (r < 0)
		cprintf("priotest: OK, no se pudo aumentar la propia prioridad "
		        "(r=%d)\n",
		        r);
	else
		cprintf("priotest: ERROR, se pudo aumentar la propia "
		        "prioridad\n");

	// Security rule: it can lower it. Lowering it below every child is
	// also what releases them: until this point the parent outranked all
	// of them and none had made progress.
	own = base - NCHILD - 1;
	r = sys_setpriority(0, own);
	if (r == 0)
		cprintf("priotest: OK, se pudo reducir la propia prioridad a "
		        "%d; arrancan los hijos\n",
		        own);
	else
		cprintf("priotest: ERROR, no se pudo reducir la propia "
		        "prioridad (r=%d)\n",
		        r);

	// Wait for the children in the order they were created, which is also
	// the order of decreasing priority. The parent is now the lowest
	// priority in the system, so the only reason it gets the CPU back at
	// all is aging.
	for (i = 0; i < NCHILD; i++)
		while (alive(children[i]))
			sys_yield();

	cprintf("priotest: fin del proceso padre\n");
}
