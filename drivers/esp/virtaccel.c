#if defined(CONFIG_VIRTUAL_ACCELERATORS) && CONFIG_VIRTUAL_ACCELERATORS == 1

#include <linux/syscalls.h>
#include <linux/list.h>

#include <linux/esp/esp.h>
#include <linux/esp/virtaccel.h>
// #include <linux/esp/libesp.h>

extern bool esp_xfer_input_ok(struct esp_device *esp, const struct contig_desc *contig);
extern void esp_runtime_config(struct esp_device *esp);
extern int esp_flush(struct esp_device *esp);
extern void esp_transfer(struct esp_device *esp, const struct contig_desc *contig);
extern void esp_run(struct esp_device *esp);
extern int esp_wait(struct esp_device *esp);
extern void esp_update_status(struct esp_device *esp);
extern long esp_p2p_init(struct esp_device *esp, struct esp_access *access);

char accel_name[64] = "mac_stratus";
module_param_string(accel_name, accel_name, sizeof(accel_name), 0644);
/* change by: echo -n "<name>" | tee /sys/module/virtaccel/parameters/accel_name */

// static struct esp_device *mac_device;

SYSCALL_DEFINE1(clone_accel, struct esp_access *, accel_info_user) {
    /*
     * This system call is to start a task the mac accelerator
     * Equivalent to esp_run(cfg, 1), which mainly consists of an open() call and an ioctl() call.
     * There will be a kernel-level queue to cache the ready tasks.
     * The kernel-level code is now responsible for dispatching the tasks to the accelerator.
     */

    extern struct list_head esp_devices;
    extern struct esp_status esp_status;

    struct esp_device *dev = NULL, *entry;
    struct esp_access *access;
	struct contig_desc *contig;
    int rc;

    /**
     * esp_run(cfg, 1) => esp_run_parallel(cfg[], [1], [1]);
     *   esp_config(cfg[], 1, [1]): keep it in user-space;
     *   open(esp_thread_info_t::devname, O_RDWR, 0) => esp_open() in esp.ko;
     *   accelerator_thread_serial => ioctl(info->fd, info->ioctl_req, info->esp_desc) => esp_ioctl() in esp.ko;
     */
    // if (unlikely(!mac_device)) {                                                    /* obtain mac_device without esp_open() */
    struct list_head *node;
    
    list_for_each(node, &esp_devices) {
        entry = list_entry(node, struct esp_device, list);                          /* find mac_stratus device */
        if (!strncmp(entry->driver->plat.driver.name, accel_name, strlen(accel_name))) {
            dev = entry;
            break;
        }
    }
    if (unlikely(!dev)) {
        // printk(KERN_ERR "clone_accel: cannot find %s device\n", accel_name);
        return -ENODEV;
    }
    // }

    // printk(KERN_INFO "clone_accel: mac_stratus device found at 0x%llx\n", (unsigned long long)mac_device);
    // printk(KERN_INFO "clone_accel: mac_stratus iomem at 0x%llx\n", (unsigned long long)mac_device->iomem);

    /* esp_open() */
    if (!try_module_get(dev->module)) {
        return -ENODEV;
    }

    /* esp_ioctl() */
    access = kmalloc(dev->driver->arg_size, GFP_KERNEL);
    if (access == NULL) {
        module_put(dev->module);
        return -ENOMEM;
    }

    if (copy_from_user(access, accel_info_user, dev->driver->arg_size)) {
        rc = -EFAULT;
        goto clone_accel_out;
    }

	contig = contig_khandle_to_desc(access->contig);
	if (contig == NULL) {
		rc = -EFAULT;
		goto clone_accel_out;
	}
	if (access->p2p_nsrcs > 4) {
		rc = -EINVAL;
		goto clone_accel_out;
	}

	if (!esp_xfer_input_ok(dev, contig)) {
		rc = -EINVAL;
		goto clone_accel_out;
	}

	if (dev->driver->xfer_input_ok && !dev->driver->xfer_input_ok(dev, access)) {
		rc = -EINVAL;
		goto clone_accel_out;
	}

	if (mutex_lock_interruptible(&dev->lock)) {
		rc = -EINTR;
		goto clone_accel_out;
	}

	rc = esp_p2p_init(dev, access);
	if (rc)
		goto clone_accel_out;

	dev->coherence = access->coherence;
	dev->footprint = access->footprint;
    dev->alloc_policy = access->alloc_policy;
    dev->ddr_node = access->ddr_node;
	dev->in_place = access->in_place;
	dev->reuse_factor = access->reuse_factor;

    if (mutex_lock_interruptible(&esp_status.lock)) {
        rc = -EINTR;
        goto clone_accel_out;
    }

    esp_runtime_config(dev);

    mutex_unlock(&esp_status.lock);

	rc = esp_flush(dev);
	if (rc)
		goto clone_accel_out;

	esp_transfer(dev, contig);

	if (dev->driver->prep_xfer)
		dev->driver->prep_xfer(dev, access);

	if (access->run) {
        if (access->start_stop) {
           esp_run(dev);
        } else {
           esp_run(dev);
           rc = esp_wait(dev);
        }
	}

    if (mutex_lock_interruptible(&esp_status.lock)) {
        rc = -EINTR;
        goto clone_accel_out;
    }

    esp_update_status(dev);

    mutex_unlock(&esp_status.lock);

	mutex_unlock(&dev->lock);

clone_accel_out:
    module_put(dev->module);
	kfree(access);
	return rc;
}

#endif
