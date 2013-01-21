/*
 * machine_kexec.c - handle transition of Linux booting another kernel
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/kexec.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/kallsyms.h>

#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>
#include <asm/mach-types.h>

extern const unsigned char relocate_new_kernel[];
extern const unsigned int relocate_new_kernel_size;

//extern void setup_mm_for_reboot(char mode);
static void (*kexec_setup_mm_for_reboot)(char mode);
void (*kexec_gic_raise_softirq)(const struct cpumask *mask, unsigned int irq);
int (*kexec_msm_pm_wait_cpu_shutdown)(unsigned int cpu);

extern unsigned long kexec_start_address;
extern unsigned long kexec_indirection_page;
extern unsigned long kexec_mach_type;
extern unsigned long kexec_boot_atags;

void kexec_cpu_v7_proc_fin(void);

/* Using cleaned up kexec code from 3.4 kernel here:
 * [arch/arm/kernel/ process.c]
 * http://git.kernel.org/?p=linux/kernel/git/stable/linux-stable.git;a=blob_plain;f=arch/arm/kernel/ process.c;h=48f36246a5d7a2c07715dee19f034a49934da8bc;hb=0ba1cd8da86b7c4717852e786bacc7154b62d95c
 * and
 * [arch/arm/kernel/machine_kexec.c]
 * http://git.kernel.org/?p=linux/kernel/git/stable/linux-stable.git;a=blob;f=arch/arm/kernel/machine_kexec.c;h=dfcdb9f7c1261143f93c31846ed372269a4b4783;hb=0ba1cd8da86b7c4717852e786bacc7154b62d95c
 */

extern void call_with_stack(void (*fn)(void *), void *arg, void *sp);
typedef void (*phys_reset_t)(unsigned long);

/*
 * A temporary stack to use for CPU reset. This is static so that we
 * don't clobber it with the identity mapping. When running with this
 * stack, any references to the current task *will not work* so you
 * should really do as little as possible before jumping to your reset
 * code.
 */
static u64 soft_restart_stack[256];

static void __soft_restart(void *addr)
{
	phys_reset_t phys_reset = (phys_reset_t)addr;

	/* Take out a flat memory mapping. */
	// Add the unused "mode" for 3.0 kernel
	kexec_setup_mm_for_reboot(0);

	/* Clean and invalidate caches */
	flush_cache_all();

	outer_flush_all();
	outer_disable();

	/* Turn off caching */
	kexec_cpu_v7_proc_fin();

	/* Push out any further dirty data, and ensure cache is empty */
	flush_cache_all();

	/* Switch to the identity mapping. */
//	phys_reset = (phys_reset_t)(unsigned long)virt_to_phys(kexec_cpu_v7_reset);
//	phys_reset((unsigned long)addr);
	phys_reset(0);

	/* Should never get here. */
	BUG();
}

void soft_restart(unsigned long addr)
{
	u64 *stack = soft_restart_stack + ARRAY_SIZE(soft_restart_stack);
	kexec_setup_mm_for_reboot = (void *)kallsyms_lookup_name("setup_mm_for_reboot");

	/* Disable interrupts first */
	local_irq_disable();
	local_fiq_disable();

	/* Disable the L2 if we're the last man standing. */
	printk(KERN_EMERG "soft_restart: num_online_cpus == %d\n", num_online_cpus());
	if (num_online_cpus() == 1)
		outer_disable();

	printk(KERN_EMERG "Bye\n");

	/* Change to the new stack and continue with the reset. */
	call_with_stack(__soft_restart, (void *)addr, (void *)stack);

	/* Should never get here. */
	BUG();
}

// static atomic_t waiting_for_crash_ipi;

/*
 * Provide a dummy crash_notes definition while crash dump arrives to arm.
 * This prevents breakage of crash_notes attribute in kernel/ksysfs.c.
 */

int machine_kexec_prepare(struct kimage *image)
{
	return 0;
}
EXPORT_SYMBOL(machine_kexec_prepare);

void machine_kexec_cleanup(struct kimage *image)
{
}
EXPORT_SYMBOL(machine_kexec_cleanup);

enum ipi_msg_type {
	IPI_CPU_START = 1,
	IPI_TIMER = 2,
	IPI_RESCHEDULE,
	IPI_CALL_FUNC,
	IPI_CALL_FUNC_SINGLE,
	IPI_CPU_STOP,
	IPI_CPU_BACKTRACE,
};

static void kexec_smp_kill_cpus(cpumask_t *mask)
{
	unsigned int cpu;
	for_each_cpu(cpu, mask) {
		kexec_msm_pm_wait_cpu_shutdown(cpu);
	}
}

void machine_shutdown(void)
{
	unsigned long timeout;
	struct cpumask mask;

	kexec_gic_raise_softirq = (void *)kallsyms_lookup_name("gic_raise_softirq");
	kexec_msm_pm_wait_cpu_shutdown = (void *)kallsyms_lookup_name("msm_pm_wait_cpu_shutdown");
	if (!kexec_msm_pm_wait_cpu_shutdown) {
		printk(KERN_EMERG "msm_pm_wait_cpu_shutdown NOT FOUND!\n");
		return;
	}

	if (kexec_gic_raise_softirq) {
		printk(KERN_EMERG "found gic_raise_softirq: %p\n", kexec_gic_raise_softirq);

		cpumask_copy(&mask, cpu_online_mask);
		cpumask_clear_cpu(smp_processor_id(), &mask);
		if (!cpumask_empty(&mask)) {
			printk(KERN_EMERG "Sending STOP to extra CPUs ...\n");
			kexec_gic_raise_softirq(&mask, IPI_CPU_STOP);
		}

		/* Wait up to five seconds for other CPUs to stop */
		timeout = USEC_PER_SEC;
		printk(KERN_EMERG "waiting for CPUs ...(%lu)\n", timeout);
		while (num_online_cpus() > 1 && timeout--)
			udelay(1);

		if (num_online_cpus() > 1)
			pr_warning("SMP: failed to stop secondary CPUs\n");

		kexec_smp_kill_cpus(&mask);
	}
	else {
		pr_warning("SMP: failed to stop secondary CPUs\n");
	}

#if 0
	void (*kexec_smp_send_stop)(void);
	kexec_smp_send_stop = (void *)kallsyms_lookup_name("smp_send_stop");
	if (!kexec_smp_send_stop) {
		printk(KERN_EMERG "kexec_smp_send_stop NOT FOUND!\n");
		return;
	}
	kexec_smp_send_stop();
#endif
}
EXPORT_SYMBOL(machine_shutdown);

void machine_crash_nonpanic_core(void *unused)
{
#if 0
	struct pt_regs regs;

	crash_setup_regs(&regs, NULL);
	printk(KERN_DEBUG "CPU %u will stop doing anything useful since another CPU has crashed\n",
	       smp_processor_id());
	crash_save_cpu(&regs, smp_processor_id());
	flush_cache_all();

	atomic_dec(&waiting_for_crash_ipi);
	while (1)
		cpu_relax();
#endif
}

void machine_crash_shutdown(struct pt_regs *regs)
{
#if 0
	unsigned long msecs;

	local_irq_disable();

	atomic_set(&waiting_for_crash_ipi, num_online_cpus() - 1);
	smp_call_function(machine_crash_nonpanic_core, NULL, false);
	msecs = 1000; /* Wait at most a second for the other cpus to stop */
	while ((atomic_read(&waiting_for_crash_ipi) > 0) && msecs) {
		mdelay(1);
		msecs--;
	}
	if (atomic_read(&waiting_for_crash_ipi) > 0)
		printk(KERN_WARNING "Non-crashing CPUs did not react to IPI\n");

	crash_save_cpu(regs, smp_processor_id());

	printk(KERN_INFO "Loading crashdump kernel...\n");
#endif
}

/*
 * Function pointer to optional machine-specific reinitialization
 */
void (*kexec_reinit)(void);

void machine_kexec(struct kimage *image)
{
	unsigned long page_list;
	unsigned long reboot_code_buffer_phys;
	void *reboot_code_buffer;

	arch_kexec();

	page_list = image->head & PAGE_MASK;

	/* we need both effective and real address here */
	reboot_code_buffer_phys =
	    page_to_pfn(image->control_code_page) << PAGE_SHIFT;
	reboot_code_buffer = page_address(image->control_code_page);

	printk(KERN_EMERG "va: %08x\n", (int)reboot_code_buffer);
	printk(KERN_EMERG "pa: %08x\n", (int)reboot_code_buffer_phys);

	/* Prepare parameters for reboot_code_buffer*/
	kexec_start_address = image->start;
	kexec_indirection_page = page_list;
	kexec_mach_type = machine_arch_type;
	kexec_boot_atags = image->start - KEXEC_ARM_ZIMAGE_OFFSET + KEXEC_ARM_ATAGS_OFFSET;

	printk(KERN_EMERG "kexec_start_address: %08lx\n", kexec_start_address);
	printk(KERN_EMERG "kexec_indirection_page: %08lx\n", kexec_indirection_page);
	printk(KERN_EMERG "kexec_mach_type: %08lx\n", kexec_mach_type);
	printk(KERN_EMERG "kexec_boot_atags: %08lx\n", kexec_boot_atags);

	/* copy our kernel relocation code to the control code page */
	memcpy(reboot_code_buffer,
	       relocate_new_kernel, relocate_new_kernel_size);


	flush_icache_range((unsigned long) reboot_code_buffer,
			   (unsigned long) reboot_code_buffer + KEXEC_CONTROL_PAGE_SIZE);

	if (kexec_reinit)
		kexec_reinit();
	soft_restart(reboot_code_buffer_phys);
#if 0
	local_irq_disable();
	local_fiq_disable();
	setup_mm_for_reboot(0); /* mode is not used, so just pass 0*/
	flush_cache_all();
	outer_flush_all();
	outer_disable();
	cpu_proc_fin();
	outer_inv_all();
	flush_cache_all();
	__virt_to_phys(cpu_reset)(reboot_code_buffer_phys);
#endif
}
EXPORT_SYMBOL(machine_kexec);

MODULE_LICENSE("GPL");
