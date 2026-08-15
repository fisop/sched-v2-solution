// Part 2: test for sys_sleep.
// Checks that processes go to sleep and wake up correctly.

#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	int i;
	envid_t child;

	cprintf("[%08x] sleeptest starting\n", thisenv->env_id);

	for (i = 0; i < 3; i++) {
		child = fork();
		if (child == 0) {
			uint32_t sleep_ticks = (i + 1) * 5;
			cprintf("[%08x] child %d sleeping for %d ticks\n",
			        thisenv->env_id,
			        i,
			        sleep_ticks);
			sys_sleep(sleep_ticks);
			cprintf("[%08x] child %d woke up!\n", thisenv->env_id, i);
			exit();
		}
	}

	// The parent also sleeps for a bit
	cprintf("[%08x] parent sleeping for 3 ticks\n", thisenv->env_id);
	sys_sleep(3);
	cprintf("[%08x] parent awake\n", thisenv->env_id);
}
