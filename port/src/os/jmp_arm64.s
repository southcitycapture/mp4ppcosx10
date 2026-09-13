/* gcsetjmp / gclongjmp for arm64 macOS -- the host-native build.
 *
 * The host build exists so the port can be developed and compared against
 * Dolphin on the Mac, and it has one problem the G4 does not: the game stores
 * code and stack pointers in the u32 fields `jmp_buf.lr` and `jmp_buf.sp`
 * (and in `HUPROCESS.base_sp`), which truncates a 64-bit address.
 *
 * The port makes that lossless rather than working around it:
 *
 *  - every stack the game ever runs on lives in one mmap'd region -- MEM1 plus
 *    the port's own game stack, allocated contiguously and checked not to
 *    straddle a 4 GB boundary (port/src/os/os_arena.c), and the whole game runs
 *    on it via port_call_on_stack -- so `port_stack_base_hi | sp_lo` is exact;
 *  - every `lr` is a code address in the one main image, so
 *    `port_text_base_hi | lr_lo` is exact.
 *
 * jmp_buf is 248 bytes (see include/game/jmp.h).  The arm64 callee-saved set
 * is x19-x28, x29 and d8-d15 = 152 bytes, which fits exactly in the space the
 * PowerPC layout gives f14-f31 at offset 96.
 */
	.text
	.align 2

	.globl _gcsetjmp
_gcsetjmp:
	mov	x9, sp
	str	w9, [x0, #8]
	str	w30, [x0, #0]
	stp	x19, x20, [x0, #96]
	stp	x21, x22, [x0, #112]
	stp	x23, x24, [x0, #128]
	stp	x25, x26, [x0, #144]
	stp	x27, x28, [x0, #160]
	str	x29, [x0, #176]
	stp	d8, d9, [x0, #184]
	stp	d10, d11, [x0, #200]
	stp	d12, d13, [x0, #216]
	stp	d14, d15, [x0, #232]
	mov	w0, #0
	ret

	.globl _gclongjmp
_gclongjmp:
	ldp	x19, x20, [x0, #96]
	ldp	x21, x22, [x0, #112]
	ldp	x23, x24, [x0, #128]
	ldp	x25, x26, [x0, #144]
	ldp	x27, x28, [x0, #160]
	ldr	x29, [x0, #176]
	ldp	d8, d9, [x0, #184]
	ldp	d10, d11, [x0, #200]
	ldp	d12, d13, [x0, #216]
	ldp	d14, d15, [x0, #232]
	ldr	w9, [x0, #8]			/* sp, low 32 bits  */
	ldr	w10, [x0, #0]			/* lr, low 32 bits  */
	adrp	x11, _port_stack_base_hi@GOTPAGE
	ldr	x11, [x11, _port_stack_base_hi@GOTPAGEOFF]
	ldr	x11, [x11]
	orr	x9, x11, x9
	mov	sp, x9
	adrp	x12, _port_text_base_hi@GOTPAGE
	ldr	x12, [x12, _port_text_base_hi@GOTPAGEOFF]
	ldr	x12, [x12]
	orr	x10, x12, x10
	mov	x30, x10
	cmp	w1, #0
	csinc	w0, w1, wzr, ne
	br	x10

/* void port_call_on_stack(void (*fn)(void), void *stack_top) */
	.globl _port_call_on_stack
_port_call_on_stack:
	and	x2, x1, #~15
	mov	x9, x0
	mov	sp, x2
	mov	x29, #0
	mov	x30, #0
	blr	x9
	brk	#0
