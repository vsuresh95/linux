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

static struct esp_device *mac_device;

SYSCALL_DEFINE1(clone_accel, struct esp_access *, accel_info_user) {
    /*
     * This system call is to start a task the mac accelerator
     * Equivalent to esp_run(cfg, 1), which mainly consists of an open() call and an ioctl() call.
     * There will be a kernel-level queue to cache the ready tasks.
     * The kernel-level code is now responsible for dispatching the tasks to the accelerator.
     */

    extern struct list_head esp_devices;
    extern struct esp_status esp_status;

    struct esp_device *dev;
    struct esp_access *access;
	struct contig_desc *contig;
    int rc;

    /**
     * esp_run(cfg, 1) => esp_run_parallel(cfg[], [1], [1]);
     *   esp_config(cfg[], 1, [1]): keep it in user-space;
     *   open(esp_thread_info_t::devname, O_RDWR, 0) => esp_open() in esp.ko;
     *   accelerator_thread_serial => ioctl(info->fd, info->ioctl_req, info->esp_desc) => esp_ioctl() in esp.ko;
     */
    if (unlikely(!mac_device)) {                                                    /* obtain mac_device without esp_open() */
        struct list_head *node;
        
        list_for_each(node, &esp_devices) {
            dev = list_entry(node, struct esp_device, list);                        /* find mac_stratus device */
            if (!strncmp(dev->driver->plat.driver.name, "mac_stratus", strlen("mac_stratus"))) {
                mac_device = dev;
                break;
            }
        }
        if (unlikely(!mac_device)) {
            // printk(KERN_ERR "clone_accel: cannot find mac_stratus device\n");
            return -ENODEV;
        }
    }

    // printk(KERN_INFO "clone_accel: mac_stratus device found at 0x%llx\n", (unsigned long long)mac_device);
    // printk(KERN_INFO "clone_accel: mac_stratus iomem at 0x%llx\n", (unsigned long long)mac_device->iomem);

    /* esp_open() */
    if (!try_module_get(mac_device->module)) {
        return -ENODEV;
    }

    /* esp_ioctl() */
    access = kmalloc(mac_device->driver->arg_size, GFP_KERNEL);
    if (access == NULL) {
        module_put(mac_device->module);
        return -ENOMEM;
    }

    if (copy_from_user(access, accel_info_user, mac_device->driver->arg_size)) {
        kfree(access);
        module_put(mac_device->module);
        return -EFAULT;
    }

	contig = contig_khandle_to_desc(access->contig);
	if (contig == NULL) {
		rc = -EFAULT;
		goto out;
	}
	if (access->p2p_nsrcs > 4) {
		rc = -EINVAL;
		goto out;
	}

	if (!esp_xfer_input_ok(mac_device, contig)) {
		rc = -EINVAL;
		goto out;
	}

	if (mac_device->driver->xfer_input_ok && !mac_device->driver->xfer_input_ok(mac_device, access)) {
		rc = -EINVAL;
		goto out;
	}

	if (mutex_lock_interruptible(&mac_device->lock)) {
		rc = -EINTR;
		goto out;
	}

	rc = esp_p2p_init(mac_device, access);
	if (rc)
		goto out;

	mac_device->coherence = access->coherence;
	mac_device->footprint = access->footprint;
    mac_device->alloc_policy = access->alloc_policy;
    mac_device->ddr_node = access->ddr_node;
	mac_device->in_place = access->in_place;
	mac_device->reuse_factor = access->reuse_factor;

    if (mutex_lock_interruptible(&esp_status.lock)) {
        rc = -EINTR;
        goto out;
    }

    esp_runtime_config(mac_device);

    mutex_unlock(&esp_status.lock);

	rc = esp_flush(mac_device);
	if (rc)
		goto out;

	esp_transfer(mac_device, contig);

	if (mac_device->driver->prep_xfer)
		mac_device->driver->prep_xfer(mac_device, access);

	if (access->run) {
        if (access->start_stop) {
           esp_run(mac_device);
        } else {
           esp_run(mac_device);
           rc = esp_wait(mac_device);
        }
	}

    if (mutex_lock_interruptible(&esp_status.lock)) {
        rc = -EINTR;
        goto out;
    }

    esp_update_status(mac_device);

    mutex_unlock(&esp_status.lock);

	mutex_unlock(&mac_device->lock);

out:
    module_put(mac_device->module);
	kfree(access);
	return rc;
}

#endif
