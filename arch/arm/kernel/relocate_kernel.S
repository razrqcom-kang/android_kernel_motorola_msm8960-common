/*
 * relocate_kernel.S - put the kernel image in place to boot
 */

#include <asm/kexec.h>
#include <linux/linkage.h>
#include <asm/assembler.h>

ENTRY(kexec_redo)
	movw	r3, #:lower16:0x00000000
	movt	r3, #:upper16:0x00000000
	movw	r4, #:lower16:0x00000001
	movt	r4, #:upper16:0x00000001
	str	r4, [r3]
ENDPROC(kexec_redo)

/*
 * void call_with_stack(void (*fn)(void *), void *arg, void *sp)
 *
 * Change the stack to that pointed at by sp, then invoke fn(arg) with
 * the new stack.
 */
ENTRY(kexec_call_with_stack)
	str	sp, [r2, #-4]!
	str	lr, [r2, #-4]!

	mov	sp, r2
	mov	r2, r0
	mov	r0, r1

	adr	lr, BSYM(1f)
	mov	pc, r2

1:	ldr	lr, [sp]
	ldr	sp, [sp, #4]
	mov	pc, lr
ENDPROC(kexec_call_with_stack)

ENTRY(kexec_cpu_v7_proc_fin)
	mrc	p15, 0, r0, c1, c0, 0		@ ctrl register
	bic	r0, r0, #0x1000			@ ...i............
	bic	r0, r0, #0x0006			@ .............ca.
	mcr	p15, 0, r0, c1, c0, 0		@ disable caches
	mov	pc, lr
ENDPROC(kexec_cpu_v7_proc_fin)

//	.align	5
ENTRY(kexec_cpu_v7_reset)
	mrc     p15, 0, r1, c1, c0, 0           @ ctrl register
	bic     r1, r1, #0x0001                 @ ...............m
        mcr     p15, 0, r1, c1, c0, 0           @ Turn off MMU
        mcr     p15, 0, ip, c8, c7, 0           @ invalidate I & D,flush TLB
        mcr     p15, 0, ip, c7, c5, 6           @ flush BTC
        dsb
        isb
	mov     pc,r0
ENDPROC(kexec_cpu_v7_reset)

	.align 8
	.balign 32
	.globl relocate_new_kernel
relocate_new_kernel:
//	@ reset
//	mrc     p15, 0, r1, c1, c0, 0           @ ctrl register
//	bic     r1, r1, #0x0001                 @ ...............m
//	mcr     p15, 0, r1, c1, c0, 0           @ Turn off MMU
//	dsb
//	isb
//	mcr     p15, 0, ip, c8, c7, 0           @ invalidate I & D,flush TLB
//	mcr     p15, 0, ip, c7, c5, 6           @ flush BTC
//	mov     pc,r0
//	dsb
//	isb
//
	ldr	r0,kexec_indirection_page
	ldr	r1,kexec_start_address

	/*
	 * If there is no indirection page (we are doing crashdumps)
	 * skip any relocation.
	 */
//	cmp	r0, #0
//	beq	2f

0:	/* top, read another word for the indirection page */
	ldr	r3, [r0],#4

	/* Is it a destination page. Put destination address to r4 */
	tst	r3,#1,0
	beq	1f
	bic	r4,r3,#1
	b	0b
1:
	/* Is it an indirection page */
	tst	r3,#2,0
	beq	1f
	bic	r0,r3,#2
	b	0b
1:

	/* are we done ? */
	tst	r3,#4,0
	beq	1f
	b	2f

1:
	/* is it source ? */
	tst	r3,#8,0
	beq	0b
	bic r3,r3,#8
	mov r6,#1024
9:
	ldr r5,[r3],#4
	str r5,[r4],#4
	subs r6,r6,#1
	bne 9b
	b 0b

2:
	/* Jump to relocated kernel */
	mov lr,r1
	mov r0,#0
	ldr r1,kexec_mach_type
	ldr r2,kexec_boot_atags
	mov pc,lr

	.align

	.globl kexec_start_address
kexec_start_address:
	.long	0x0

	.globl kexec_indirection_page
kexec_indirection_page:
	.long	0x0

	.globl kexec_mach_type
kexec_mach_type:
	.long	0x0

	/* phy addr of the atags for the new kernel */
	.globl kexec_boot_atags
kexec_boot_atags:
	.long	0x0

relocate_new_kernel_end:

	.globl relocate_new_kernel_size
relocate_new_kernel_size:
	.long relocate_new_kernel_end - relocate_new_kernel


