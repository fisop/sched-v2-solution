/*
 * context_switch(struct Trapframe *tf)
 *
 * Restores the CPU state saved in 'tf' and jumps to user mode with iret.
 * Never returns.
 *
 * The trick is moving esp to the start of the Trapframe: from there, each pop
 * consumes one struct field in order, and by the time it reaches tf_eip the
 * stack has exactly the layout iret expects (eip, cs, eflags, esp, ss).
 */

.globl context_switch;
context_switch:
	movl 4(%esp), %esp	/* esp points to the Trapframe (only argument) */

	popal			/* struct PushRegs, in the reverse order of pushal */
	popl %es		/* tf_es  + tf_padding1 */
	popl %ds		/* tf_ds  + tf_padding2 */
	addl $0x8, %esp		/* skip tf_trapno and tf_err */

	/* Now esp points to tf_eip: iret pops eip, cs and eflags, and since the
	 * switch is from ring 0 to ring 3 it also pops esp and ss. */
	iret

spin:
	jmp spin
