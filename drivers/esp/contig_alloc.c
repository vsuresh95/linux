#include <linux/esp/contig_alloc.h>

#include <linux/printk.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/device.h>

#ifdef CONFIG_VIRTUAL_ACCELERATORS

#define PFX "contig_alloc: "
#define MAX_DDR_NODES 8

static unsigned long chunk_size;
unsigned long contig_chunk_size_log = 20;
EXPORT_SYMBOL_GPL(contig_chunk_size_log);
module_param_named(chunk_log, contig_chunk_size_log, ulong, S_IRUGO);
static unsigned int nddr = 1;
module_param(nddr, uint, S_IRUGO);
static unsigned long mem_start[MAX_DDR_NODES] = { 0xa0200000 };
module_param_array_named(start, mem_start, ulong, &nddr, S_IRUGO);
static unsigned long mem_size[MAX_DDR_NODES] = { 0x1fe00000 };
module_param_array_named(size, mem_size, ulong, &nddr, S_IRUGO);

static struct class *contig_class;
static DEFINE_MUTEX(contig_lock);
static LIST_HEAD(desc_list);
static struct list_head inactive_chunks[MAX_DDR_NODES];
static unsigned long mem_allocated[MAX_DDR_NODES];
static caddr_t bp_buf __maybe_unused;

static const struct file_operations contig_fops = {
	.owner		= THIS_MODULE,
	// .open		= contig_open,
	// .release	= contig_release,
	// .unlocked_ioctl	= contig_ioctl,
	// .mmap		= contig_mmap,
};

static int contig_create_file(void)
{
    printk(KERN_INFO "Creating contig_alloc device file\n");
	contig_class = class_create(THIS_MODULE, "contig_alloc");
    printk(KERN_INFO "contig_class=0x%llx\n", (uint64_t)contig_class);
	if (IS_ERR(contig_class))
		return PTR_ERR(contig_class);

	if (register_chrdev(CONTIG_MAJOR, "contig_alloc", &contig_fops))
		goto err_chrdev;

	if (IS_ERR(device_create(contig_class, NULL, MKDEV(CONTIG_MAJOR, CONTIG_MINOR), NULL, "contig_alloc")))
		goto err_device_create;

	return 0;

 err_device_create:
    printk(KERN_ERR "Failed to create contig_alloc device\n");
	unregister_chrdev(CONTIG_MAJOR, "contig_alloc");
 err_chrdev:
	printk(KERN_ERR "Failed to create contig_alloc device file\n");
	class_destroy(contig_class);
	return -ENODEV;
}

static void contig_remove_file(void)
{
	device_destroy(contig_class, MKDEV(CONTIG_MAJOR, CONTIG_MINOR));
	unregister_chrdev(CONTIG_MAJOR, "contig_alloc");
	class_destroy(contig_class);
}

int contig_init(void) {
    int i;
    int rc;

    printk(KERN_INFO "Initializing contig_alloc\n");
    if (contig_chunk_size_log >= 32)
		return -EINVAL;
    printk(KERN_INFO "contig_chunk_size_log=%lu\n", contig_chunk_size_log);
	chunk_size = BIT(contig_chunk_size_log);
    printk(KERN_INFO "chunk_size=%lu\n", chunk_size);

#ifndef CONFIG_BIGPHYS_AREA
	if (!mem_start[0])
		return -EINVAL;
#endif
	if (chunk_size < PAGE_SIZE) {
		pr_err(PFX "chunk_size (0x%lx) < PAGE_SIZE (0x%lx)\n",
			chunk_size, PAGE_SIZE);
		return -EINVAL;
	}

    for (i = 0; i < nddr; i++) {
		INIT_LIST_HEAD(&inactive_chunks[i]);
		if (mem_size[i] % chunk_size) {
			pr_warn(PFX "chunk_size (0x%lx) does not divide evenly mem_size[%d] (0x%lx); discarding %ld bytes\n",
				chunk_size, i, mem_size[i], mem_size[i] % chunk_size);
			mem_size[i] -= mem_size[i] % chunk_size;
		} else {
            printk(KERN_INFO "mem_size[%d]=0x%lx\n", i, mem_size[i]);
        }
		if (!mem_size[i] || !chunk_size)
			return -EINVAL;
		if (chunk_size > mem_size[i]) {
			pr_err(PFX "chunk_size (0x%lx) > mem_size[%d] (0x%lx)\n",
				chunk_size, i, mem_size[i]);
			return -EINVAL;
		}
	}

	rc = contig_create_file();
	if (rc)
		return rc;

    return 0;
}

void contig_exit(void)
{
    contig_remove_file();
    return;
}

#endif /* CONFIG_VIRTUAL_ACCELERATORS */
