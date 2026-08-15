/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>


static int
check_perm(int perm, pte_t *pte)
{
	if (perm & (~PTE_SYSCALL))
		return -E_INVAL;

	if (!(perm & PTE_P) || !(perm & PTE_U))
		return -E_INVAL;

	if (pte) {
		if (*pte && !(*pte & PTE_P))
			return -E_INVAL;

		if ((perm & PTE_W) && !(*pte & PTE_W))
			return -E_INVAL;
	}
	return 0;
}


/*
 * Funcion auxiliar para alocar una pagina fisica en el pgdir
 * del entorno env. Se mappea a la direccion virtual va con
 * permisos (perm | PTE_P).
 *
 * Importante: NO realiza chequeos de permisos.
 *
 * Devuelve 0 si no ocurreiron errores, y un valor < 0 en caso
 * de error.
 */
static int
env_page_alloc(struct Env *env, void *va, int perm)
{
	int err = check_perm(perm, NULL);
	if (err < 0)
		return err;

	struct PageInfo *p = page_alloc(ALLOC_ZERO);
	if (!p)
		return -E_NO_MEM;

	return page_insert(env->env_pgdir, p, va, perm);
}


static int
env_page_map(struct Env *srcenv, void *srcva, struct Env *dstenv, void *dstva, int perm)
{
	pte_t *srcpte;
	struct PageInfo *page = page_lookup(srcenv->env_pgdir, srcva, &srcpte);
	if (!page)
		return -E_INVAL;

	if ((perm & PTE_W) & ~(PTE_W & *srcpte))
		return -E_INVAL;

	return page_insert(dstenv->env_pgdir, page, dstva, perm);
}


// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.
	user_mem_assert(curenv, s, len, PTE_P | PTE_W | PTE_U);

	// Print the string supplied by the user.
	cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
	return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	if (e == curenv)
		cprintf("[%08x] exiting gracefully\n", curenv->env_id);
	else
		cprintf("[%08x] destroying %08x\n", curenv->env_id, e->env_id);

	env_destroy(e);

	return 0;
}

// Deschedule current environment and pick a different one to run.
static void
sys_yield(void)
{
	sched_yield();
}

// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
//	-E_NO_MEM on memory exhaustion.
static envid_t
sys_exofork(void)
{
	// Create the new environment with env_alloc(), from kern/env.c.
	// It should be left as env_alloc created it, except that
	// status is set to ENV_NOT_RUNNABLE, and the register set is copied
	// from the current environment -- but tweaked so sys_exofork
	// will appear to return 0.

	struct Env *newenv;
	int r;
	if ((r = env_alloc(&newenv, curenv->env_id)))
		return r;

	newenv->env_status = ENV_NOT_RUNNABLE;
	newenv->env_tf = curenv->env_tf;
	newenv->env_tf.tf_regs.reg_eax = 0;

	// Part 3: the child inherits the parent's priority.
	//
	// The full value is inherited, like the nice value in POSIX: the child
	// starts out as the parent's peer, and if the parent wants it lighter
	// it can lower its priority afterward (it can set any value on a
	// direct child). Inheriting the full value opens no security hole,
	// because nobody ends up with more priority than the parent already
	// had: to escalate you'd have to increase your own, which is exactly
	// what sys_setpriority forbids.
	newenv->env_priority = curenv->env_priority;

	return newenv->env_id;
}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
static int
sys_env_set_status(envid_t envid, int status)
{
	// Hint: Use the 'envid2env' function from kern/env.c to translate an
	// envid to a struct Env.
	// You should set envid2env's third argument to 1, which will
	// check whether the current environment has permission to set
	// envid's status.

	if ((status != ENV_RUNNABLE) && (status != ENV_NOT_RUNNABLE))
		return -E_INVAL;

	struct Env *env;
	int r;
	if ((r = envid2env(envid, &env, 1)))
		return r;

	env->env_status = status;

	return 0;
}

// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	struct Env *env;
	int r;
	if ((r = envid2env(envid, &env, 1)))
		return r;

	user_mem_assert(env, func, PGSIZE, PTE_P | PTE_U);

	env->env_pgfault_upcall = func;

	return 0;
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.  See PTE_SYSCALL in inc/mmu.h.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	// Hint: This function is a wrapper around page_alloc() and
	//   page_insert() from kern/pmap.c.
	//   Most of the new code you write should be to check the
	//   parameters for correctness.
	//   If page_insert() fails, remember to free the page you
	//   allocated!

	if (((uint32_t) va >= UTOP) || ((uint32_t) va % PGSIZE))
		return -E_INVAL;

	struct Env *env;
	int r;
	if ((r = envid2env(envid, &env, 1)))
		return r;

	return env_page_alloc(env, va, perm | PTE_U | PTE_P);
}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate any necessary page tables.
static int
sys_page_map(envid_t srcenvid, void *srcva, envid_t dstenvid, void *dstva, int perm)
{
	// Hint: This function is a wrapper around page_lookup() and
	//   page_insert() from kern/pmap.c.
	//   Again, most of the new code you write should be to check the
	//   parameters for correctness.
	//   Use the third argument to page_lookup() to
	//   check the current permissions on the page.

	// Check both va are >= UTOP and page-aligned
	if (((uint32_t) srcva >= UTOP) || ((uint32_t) srcva % PGSIZE))
		return -E_INVAL;
	if (((uint32_t) dstva >= UTOP) || ((uint32_t) dstva % PGSIZE))
		return -E_INVAL;

	int r;  // For errors

	struct Env *srcenv;
	struct Env *dstenv;

	if ((r = envid2env(srcenvid, &srcenv, 1)))
		return r;
	if ((r = envid2env(dstenvid, &dstenv, 1)))
		return r;

	return env_page_map(srcenv, srcva, dstenv, dstva, PTE_P | PTE_U | perm);
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
static int
sys_page_unmap(envid_t envid, void *va)
{
	// Hint: This function is a wrapper around page_remove().

	if (((uint32_t) va >= UTOP) || ((uint32_t) va % PGSIZE))
		return -E_INVAL;

	struct Env *env;
	int r;
	if ((r = envid2env(envid, &env, 1)))
		return r;

	page_remove(env->env_pgdir, va);

	return 0;
}

// Try to send 'value' to the target env 'envid'.
// If srcva < UTOP, then also send page currently mapped at 'srcva',
// so that receiver gets a duplicate mapping of the same page.
//
// The send fails with a return value of -E_IPC_NOT_RECV if the
// target is not blocked, waiting for an IPC.
//
// The send also can fail for the other reasons listed below.
//
// Otherwise, the send succeeds, and the target's ipc fields are
// updated as follows:
//    env_ipc_recving is set to 0 to block future sends;
//    env_ipc_from is set to the sending envid;
//    env_ipc_value is set to the 'value' parameter;
//    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
// The target environment is marked runnable again, returning 0
// from the paused sys_ipc_recv system call.  (Hint: does the
// sys_ipc_recv function ever actually return?)
//
// If the sender wants to send a page but the receiver isn't asking for one,
// then no page mapping is transferred, but no error occurs.
// The ipc only happens when no errors occur.
//
// Returns 0 on success, < 0 on error.
// Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist.
//		(No need to check permissions.)
//	-E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
//		or another environment managed to send first.
//	-E_INVAL if srcva < UTOP but srcva is not page-aligned.
//	-E_INVAL if srcva < UTOP and perm is inappropriate
//		(see sys_page_alloc).
//	-E_INVAL if srcva < UTOP but srcva is not mapped in the caller's
//		address space.
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in the
//		current environment's address space.
//	-E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	struct Env *dstenv;
	int r;
	if ((r = envid2env(envid, &dstenv, 0)))
		return r;

	if (!dstenv->env_ipc_recving)
		return -E_IPC_NOT_RECV;

	if (((uint32_t) srcva >= UTOP)) {
		dstenv->env_ipc_perm = 0;
		goto bail;
	}

	if ((uint32_t) srcva % PGSIZE)
		return -E_INVAL;
	pte_t *srcpte;
	page_lookup(curenv->env_pgdir, srcva, &srcpte);
	if (*srcpte && !(*srcpte & PTE_P))
		return -E_INVAL;
	if ((perm & PTE_W) && !(*srcpte & PTE_W))
		return -E_INVAL;

	perm |= PTE_U | PTE_P;

	if (dstenv->env_ipc_dstva) {
		if ((r = env_page_alloc(dstenv, dstenv->env_ipc_dstva, perm)) < 0)
			return r;
		if ((r = env_page_map(
		             curenv, srcva, dstenv, dstenv->env_ipc_dstva, perm)) <
		    0)
			return r;
		dstenv->env_ipc_perm = perm;
	}

bail:
	dstenv->env_ipc_from = (envid_t) curenv->env_id;
	dstenv->env_ipc_recving = false;
	dstenv->env_ipc_value = value;
	dstenv->env_tf.tf_regs.reg_eax = 0;
	dstenv->env_status = ENV_RUNNABLE;

	return 0;
}

// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
//
// If 'dstva' is < UTOP, then you are willing to receive a page of data.
// 'dstva' is the virtual address at which the sent page should be mapped.
//
// This function only returns on error, but the system call will eventually
// return 0 on success.
// Return < 0 on error.  Errors are:
//	-E_INVAL if dstva < UTOP but dstva is not page-aligned.
static int
sys_ipc_recv(void *dstva)
{
	curenv->env_ipc_recving = true;
	if (((uint32_t) dstva >= UTOP) || ((uint32_t) dstva % PGSIZE))
		return -E_INVAL;

	curenv->env_ipc_dstva = dstva;
	curenv->env_status = ENV_NOT_RUNNABLE;

	sys_yield();

	panic("sys_ipc_recv should not return!");

	return 0;
}

// Part 2: puts the current environment to sleep for 'n' timer interrupts.
// The environment becomes ENV_NOT_RUNNABLE and env_sleep_until is set to
// (global_ticks + n). The scheduler will wake it up once global_ticks >=
// env_sleep_until.
//
// Returns 0 if it went to sleep successfully; does not return until woken up.
// Returns -E_INVAL if n == 0.
static int
sys_sleep(uint32_t n)
{
	if (n == 0)
		return -E_INVAL;

	curenv->env_sleep_until = ticks + n;
	curenv->env_status = ENV_NOT_RUNNABLE;

	// Just like sys_ipc_recv: this syscall doesn't return through the
	// normal path (trap_dispatch never gets to write the return value), so
	// it has to be set by hand in the saved trapframe. When
	// sched_wakeup_sleeping() wakes the environment up, context_switch
	// will restore that eax and sys_sleep will "return" 0 in user space.
	curenv->env_tf.tf_regs.reg_eax = 0;

	sched_yield();

	panic("sys_sleep should not return!");

	return 0;
}

// Part 3: returns the priority of the environment envid.
// Returns -E_BAD_ENV if envid doesn't exist or the caller lacks permission.
static int
sys_getpriority(envid_t envid)
{
	struct Env *e;
	int r;

	// envid2env with checkperm resolves envid == 0 to the current
	// environment and only allows querying itself or a direct child;
	// any other case returns -E_BAD_ENV.
	if ((r = envid2env(envid, &e, true)) < 0)
		return r;

	return (int) e->env_priority;
}

// Part 3: sets the priority of the environment envid to 'priority'.
// A process CANNOT increase its own priority (only reduce it).
// A process CAN modify the priority of its direct children.
//
// Returns 0 on success, -E_BAD_ENV if envid doesn't exist or without
// permission, -E_INVAL if trying to increase the current process's priority.
static int
sys_setpriority(envid_t envid, uint32_t priority)
{
	struct Env *e;
	int r;

	if (priority > ENV_PRIORITY_MAX)
		return -E_INVAL;

	if ((r = envid2env(envid, &e, true)) < 0)
		return r;

	// An environment can lower its own priority but never raise it:
	// otherwise any process would seize the CPU. On its direct children
	// (the other case envid2env allows) it CAN set any value, because a
	// parent doesn't gain CPU by giving it to a child.
	if (e == curenv && priority > e->env_priority)
		return -E_INVAL;

	e->env_priority = priority;

	return 0;
}

// Part 4: creates a new thread that shares the current process's address
// space. The thread starts executing at 'entry' with the stack pointing to
// 'ustack_top'.
//
// Unlike fork, the thread does NOT have its own page directory; it uses the
// same env_pgdir as the parent process. When the parent process terminates,
// all of its threads are destroyed too.
//
// Returns the envid of the new thread on success, or < 0 on error.
// Errors:
//   -E_NO_FREE_ENV if there are no free entries in the PCB.
//   -E_INVAL if entry or ustack_top are invalid.
static envid_t
sys_thread_create(void *entry, void *ustack_top)
{
	struct Env *t;
	struct PageInfo *pp;
	envid_t owner;
	int r;

	// The thread will run in ring 3: both the entry point and the stack
	// have to fall within the part of the address space that belongs to
	// user space.
	if ((uintptr_t) entry >= UTOP || (uintptr_t) ustack_top > UTOP ||
	    (uintptr_t) ustack_top == 0)
		return -E_INVAL;

	// Every thread hangs off the process that owns the address space, even
	// ones created by another thread: this way they're all siblings and
	// env_free() finds every one of them when the process dies.
	owner = curenv->env_type == ENV_TYPE_THREAD ? curenv->env_parent_id
	                                            : curenv->env_id;

	if ((r = env_alloc(&t, owner)) < 0)
		return r;  // -E_NO_FREE_ENV

	// env_alloc() already gave the new environment its own page directory
	// (via env_setup_vm). It has to be given back, and the creator
	// process's shared instead: sharing the pgdir instead of copying it is
	// the whole difference between a thread and a fork.
	pp = pa2page(PADDR(t->env_pgdir));
	t->env_pgdir = NULL;
	page_decref(pp);

	t->env_pgdir = curenv->env_pgdir;

	// Now the page directory is referenced by two environments. Without
	// this increment, whichever of the two dies first would free the page
	// the other one is still using (it's the same reason env_setup_vm
	// keeps pp_ref for the pgdir).
	pa2page(PADDR(t->env_pgdir))->pp_ref++;

	t->env_type = ENV_TYPE_THREAD;

	// A thread inherits the priority of whoever creates it: it's another
	// unit of execution of the same process, not a new process competing
	// on its own.
	t->env_priority = curenv->env_priority;

	// env_alloc already left the trapframe ready for ring 3 (user
	// selectors and FL_IF enabled) and the rest of the registers zeroed:
	// all that's missing is telling it where to start and with what stack.
	t->env_tf.tf_eip = (uintptr_t) entry;
	t->env_tf.tf_esp = (uintptr_t) ustack_top;

	// env_alloc leaves it ENV_RUNNABLE, so the thread is already eligible
	// for the scheduler, which treats it just like any other process.
	return t->env_id;
}

// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.

	switch (syscallno) {
	case SYS_cputs:
		sys_cputs((char *) a1, a2);
		return 0;
	case SYS_getenvid:
		return sys_getenvid();
	case SYS_env_destroy:
		return sys_env_destroy(a1);
	case SYS_cgetc:
		return sys_cgetc();
	case SYS_exofork:
		return sys_exofork();
	case SYS_env_set_status:
		return sys_env_set_status(a1, a2);
	case SYS_page_alloc:
		return sys_page_alloc(a1, (void *) a2, a3);
	case SYS_page_map:
		return sys_page_map(a1, (void *) a2, a3, (void *) a4, a5);
	case SYS_page_unmap:
		return sys_page_unmap(a1, (void *) a2);
	case SYS_ipc_recv:
		return sys_ipc_recv((void *) a1);
	case SYS_ipc_try_send:
		return sys_ipc_try_send(a1, a2, (void *) a3, a4);
	case SYS_env_set_pgfault_upcall:
		return sys_env_set_pgfault_upcall(a1, (void *) a2);
	case SYS_yield:
		sys_yield();  // No return
	case SYS_sleep:
		return sys_sleep(a1);
	case SYS_getpriority:
		return sys_getpriority(a1);
	case SYS_setpriority:
		return sys_setpriority(a1, a2);
	case SYS_thread_create:
		return sys_thread_create((void *) a1, (void *) a2);
	default:
		return -E_INVAL;
	}
}
