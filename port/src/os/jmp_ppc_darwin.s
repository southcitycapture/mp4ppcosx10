/* gcsetjmp / gclongjmp for 32-bit PowerPC Darwin.
 *
 * The game's HUPROCESS coroutines (src/game/process.c) are built on these two
 * functions, written in src/game/jmp.c as Metrowerks `asm` with structure
 * member operands.  The port's mirror drops those bodies; this is the same
 * code for the Darwin ABI.  It ports nearly verbatim: on 32-bit PowerPC Darwin
 * r13-r31 are non-volatile exactly as in EABI, and r2 is volatile (so saving
 * it is harmless rather than wrong).
 *
 * jmp_buf, from include/game/jmp.h:
 *     0   u32    lr
 *     4   u32    cr
 *     8   u32    sp
 *     12  u32    r2
 *     16  u32    pad
 *     20  u32    regs[19]      r13 .. r31
 *     96  f64    flt_regs[19]  f14 .. f31, then fpscr
 *
 * Both functions are leaf and allocate no frame, so the sp they save is the
 * caller's own -- which is what HuPrcCreate assumes when it overwrites `lr`
 * with a function pointer and `sp` with a fabricated stack top.
 */
	.text
	.align 2

	.globl _gcsetjmp
_gcsetjmp:
	mflr	r5
	mfcr	r6
	stw	r5, 0(r3)
	stw	r6, 4(r3)
	stw	r1, 8(r3)
	stw	r2, 12(r3)
	stmw	r13, 20(r3)
	mffs	f0
	stfd	f14, 96(r3)
	stfd	f15, 104(r3)
	stfd	f16, 112(r3)
	stfd	f17, 120(r3)
	stfd	f18, 128(r3)
	stfd	f19, 136(r3)
	stfd	f20, 144(r3)
	stfd	f21, 152(r3)
	stfd	f22, 160(r3)
	stfd	f23, 168(r3)
	stfd	f24, 176(r3)
	stfd	f25, 184(r3)
	stfd	f26, 192(r3)
	stfd	f27, 200(r3)
	stfd	f28, 208(r3)
	stfd	f29, 216(r3)
	stfd	f30, 224(r3)
	stfd	f31, 232(r3)
	stfd	f0, 240(r3)
	li	r3, 0
	blr

	.globl _gclongjmp
_gclongjmp:
	lwz	r5, 0(r3)
	lwz	r6, 4(r3)
	mtlr	r5
	mtcrf	255, r6
	lfd	f14, 96(r3)
	lfd	f15, 104(r3)
	lfd	f16, 112(r3)
	lfd	f17, 120(r3)
	lfd	f18, 128(r3)
	lfd	f19, 136(r3)
	lfd	f20, 144(r3)
	lfd	f21, 152(r3)
	lfd	f22, 160(r3)
	lfd	f23, 168(r3)
	lfd	f24, 176(r3)
	lfd	f25, 184(r3)
	lfd	f26, 192(r3)
	lfd	f27, 200(r3)
	lfd	f28, 208(r3)
	lfd	f29, 216(r3)
	lfd	f30, 224(r3)
	lfd	f31, 232(r3)
	lfd	f0, 240(r3)
	mtfsf	255, f0
	lwz	r2, 12(r3)
	lmw	r13, 20(r3)
	lwz	r1, 8(r3)
	mr.	r3, r4
	bnelr
	li	r3, 1
	blr

/* void port_call_on_stack(void (*fn)(void), void *stack_top)
 *
 * Runs the whole game on a stack the port allocated, next to MEM1, so that
 * every stack pointer the game ever truncates into a u32 -- HUPROCESS's
 * base_sp, jmp_buf's sp, including the scheduler's own context in
 * HuPrcCall -- lives in one known 4 GB region.  On PowerPC that is free; the
 * 64-bit host build needs it. */
	.globl _port_call_on_stack
_port_call_on_stack:
	mflr	r0
	stw	r0, 8(r1)
	mr	r5, r3
	rlwinm	r1, r4, 0, 0, 27	/* 16-byte align the new stack top */
	li	r0, 0
	stwu	r0, -64(r1)		/* a linkage area for the callee */
	mtctr	r5
	bctrl
	trap
