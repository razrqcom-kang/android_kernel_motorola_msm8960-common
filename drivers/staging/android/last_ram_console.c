/* drivers/android/ram_console.c
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/platform_data/ram_console.h>
#include <linux/kallsyms.h>

#include <asm/bootinfo.h>

#define OMAP_KEXEC_RAM_CONSOLE_START_DEFAULT	(0x80000000 + SZ_256M)
#define OMAP_KEXEC_RAM_CONSOLE_SIZE_DEFAULT	SZ_1K

// Function Pointers
long (*kexec_memblock_remove)(phys_addr_t base, phys_addr_t size);

// proc entry
struct proc_dir_entry *entry;

static char *ram_console_old_log;
static size_t ram_console_old_log_size;

static char *ram_console_buffer;
static size_t ram_console_buffer_size;

#define ECC_BLOCK_SIZE CONFIG_ANDROID_RAM_CONSOLE_ERROR_CORRECTION_DATA_SIZE
#define ECC_SIZE CONFIG_ANDROID_RAM_CONSOLE_ERROR_CORRECTION_ECC_SIZE

static void __init
ram_console_save_old(char *buffer, const char *bootinfo, char *dest) {
	size_t old_log_size = 128;

	if (dest == NULL) {
		dest = kmalloc(old_log_size, GFP_KERNEL);
		if (dest == NULL) {
			printk(KERN_ERR
			       "ram_console: failed to allocate buffer\n");
			return;
		}
	}

	ram_console_old_log = dest;
	ram_console_old_log_size = old_log_size;
	memcpy(ram_console_old_log, &buffer, 128);
}

static int __init ram_console_init(char *buffer,
				   size_t buffer_size, const char *bootinfo,
				   char *old_buf)
{
	ram_console_buffer = buffer;
	ram_console_buffer_size = buffer_size;

	if (ram_console_buffer_size > buffer_size) {
		pr_err("ram_console: buffer %p, invalid size %zu, "
		       "datasize %zu\n", buffer, buffer_size,
		       ram_console_buffer_size);
		return 0;
	}

	ram_console_buffer_size -= (DIV_ROUND_UP(ram_console_buffer_size, ECC_BLOCK_SIZE) + 1) * ECC_SIZE;

	if (ram_console_buffer_size > buffer_size) {
		pr_err("ram_console: buffer %p, invalid size %zu, "
		       "non-ecc datasize %zu\n",
		       buffer, buffer_size, ram_console_buffer_size);
		return 0;
	}

	ram_console_save_old(buffer, bootinfo, old_buf);

	return 0;
}

static ssize_t kexec_ram_console_read_old(struct file *file, char __user *buf,
				    size_t len, loff_t *offset)
{
	loff_t pos = *offset;
	int i;

	if (pos >= 10)
		return 0;

	char temp[10];
	for (i = 0; i < 10; i++) {
		if (ram_console_old_log[i] == 0)
			temp[i] = '0';
		else if (ram_console_old_log[i] == 1)
			temp[i] = '1';
		else if (ram_console_old_log[i] == 2)
			temp[i] = '2';
		else if (ram_console_old_log[i] == 3)
			temp[i] = '3';
		else if (ram_console_old_log[i] == 4)
			temp[i] = '4';
		else if (ram_console_old_log[i] == 5)
			temp[i] = '5';
		else if (ram_console_old_log[i] == 6)
			temp[i] = '6';
		else temp[i] = '*';
	}
	if (copy_to_user(buf, &temp, 10))
		return -EFAULT;
	*offset += 10;
	return 10;
}

static const struct file_operations kexec_ram_console_file_ops = {
	.owner = THIS_MODULE,
	.read = kexec_ram_console_read_old,
};

static struct resource kexec_ram_console_resources[] = {
	{
		.flags  = IORESOURCE_MEM,
		.start  = OMAP_KEXEC_RAM_CONSOLE_START_DEFAULT,
		.end    = OMAP_KEXEC_RAM_CONSOLE_START_DEFAULT +
			  OMAP_KEXEC_RAM_CONSOLE_SIZE_DEFAULT - 1,
	},
};

static struct ram_console_platform_data kexec_ram_console_pdata;

static struct platform_device kexec_ram_console_device = {
	.name		= "kexec_ram_console",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(kexec_ram_console_resources),
	.resource	= kexec_ram_console_resources,
	.dev		= {
	.platform_data	= &kexec_ram_console_pdata,
	},
};

static int __init kexec_ram_console_late_init(void)
{
	int ret;
	struct resource *res;
	size_t start;
	size_t buffer_size;
	void *buffer;

	kexec_memblock_remove = (void *)kallsyms_lookup_name("memblock_remove");
	if (kexec_memblock_remove == NULL) {
		printk(KERN_ERR
		       "ram_console: failed to find memblock_remove\n");
		return 0;
	}

	printk(KERN_INFO
	       "ram_console: Using buffer settings: start=0x%x, size=%u\n",
			OMAP_KEXEC_RAM_CONSOLE_START_DEFAULT,
			OMAP_KEXEC_RAM_CONSOLE_SIZE_DEFAULT);

	/* Remove the ram console region from kernel's map */
	ret = kexec_memblock_remove(OMAP_KEXEC_RAM_CONSOLE_START_DEFAULT, OMAP_KEXEC_RAM_CONSOLE_SIZE_DEFAULT);
	if (ret) {
		pr_err("%s: unable to remove memory for ram console:"
			"start=0x%08x, size=0x%08x, ret=%d\n",
			__func__, (u32)OMAP_KEXEC_RAM_CONSOLE_START_DEFAULT, (u32)OMAP_KEXEC_RAM_CONSOLE_SIZE_DEFAULT, ret);
		return ret;
	}

	ret = platform_device_register(&kexec_ram_console_device);
	if (ret) {
		pr_err("%s: unable to register kexec ram console device:"
			"start=0x%08x, end=0x%08x, ret=%d\n",
			__func__, (u32)kexec_ram_console_resources[0].start,
			(u32)kexec_ram_console_resources[0].end, ret);
	}

	res = &kexec_ram_console_resources[0];

	buffer_size = res->end - res->start + 1;
	start = res->start;
	printk(KERN_INFO "ram_console: got buffer at %zx, size %zx\n",
	       start, buffer_size);
	buffer = ioremap(res->start, buffer_size);
	printk(KERN_INFO "ram_console: ioremap == 0x%08x\n", buffer);
	if (buffer == NULL) {
		printk(KERN_ERR "ram_console: failed to map memory\n");
		return -ENOMEM;
	}

	ram_console_init(buffer, buffer_size, NULL, NULL/* allocate */);

	entry = create_proc_entry("last_kexec_kmsg", S_IFREG | S_IRUGO, NULL);
	if (!entry) {
		printk(KERN_ERR "ram_console: failed to create proc entry\n");
		kfree(ram_console_old_log);
		ram_console_old_log = NULL;
		return 0;
	}

	entry->proc_fops = &kexec_ram_console_file_ops;
	entry->size = 10;
	return 0;
}

static void __exit kexec_ram_console_exit(void) {
	if (entry)
		remove_proc_entry("last_kexec_kmsg", NULL);
	platform_device_unregister(&kexec_ram_console_device);
	entry = NULL;
}

module_init(kexec_ram_console_late_init);
module_exit(kexec_ram_console_exit);

MODULE_LICENSE("GPL");
