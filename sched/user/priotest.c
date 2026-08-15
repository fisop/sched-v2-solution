// Part 3: test for the priority scheduler.
//
// The parent creates several children and assigns each one a different
// priority. This is allowed by sys_setpriority's rules: a process can modify
// the priority of its direct children to any value.
//
// Each child prints its progress while yielding the CPU on every iteration,
// so the order and frequency of the lines show who the scheduler favors. At
// the end, the parent checks the security rules around its own priority: it
// must not be able to raise it, but should be able to lower it.
//
// Note: priorities are assigned relative to the default priority returned by
// sys_getpriority(), so as not to depend on the value chosen in env_alloc.
// If the implementation bounds the priority range, adjust NCHILD.

#include <inc/lib.h>

#define NCHILD 4
#define NITER 5

static void
worker(int prio)
{
	int i;

	for (i = 0; i < NITER; i++) {
		cprintf("[%08x] prio=%d iteracion %d\n", thisenv->env_id, prio, i);
		sys_yield();
	}
}

void
umain(int argc, char **argv)
{
	int i, r;
	int base = sys_getpriority(0);

	cprintf("priotest: prioridad por defecto = %d\n", base);

	// Create the children, from lowest to highest priority.
	for (i = 1; i <= NCHILD; i++) {
		int prio = base + i;
		envid_t child = fork();

		if (child < 0)
			panic("priotest: fork: %e", child);

		if (child == 0) {
			// The child doesn't know the priority its parent
			// assigned it until it queries for it.
			worker(sys_getpriority(0));
			exit();
		}

		r = sys_setpriority(child, prio);
		cprintf("priotest: hijo [%08x] -> prioridad %d (r=%d)\n",
		        child,
		        prio,
		        r);
	}

	// Security rules regarding its own priority.
	r = sys_setpriority(0, base + 1);
	if (r < 0)
		cprintf("priotest: OK, no se pudo aumentar la propia prioridad "
		        "(r=%d)\n",
		        r);
	else
		cprintf("priotest: ERROR, se pudo aumentar la propia "
		        "prioridad\n");

	if (base > 0) {
		r = sys_setpriority(0, base - 1);
		if (r == 0)
			cprintf("priotest: OK, se pudo reducir la propia "
			        "prioridad a %d\n",
			        base - 1);
		else
			cprintf("priotest: ERROR, no se pudo reducir la propia "
			        "prioridad (r=%d)\n",
			        r);
	} else {
		cprintf("priotest: prioridad por defecto 0, no se puede probar "
		        "la reducción\n");
	}

	// Yield the CPU so the children finish before the parent does.
	for (i = 0; i < NCHILD * NITER; i++)
		sys_yield();

	cprintf("priotest: fin del proceso padre\n");
}
