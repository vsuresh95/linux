#include "linux/linkage.h"
#include "linux/printk.h"
#if defined(CONFIG_VIRTUAL_ACCELERATORS) && CONFIG_VIRTUAL_ACCELERATORS == 1

#include <linux/syscalls.h>

SYSCALL_DEFINE0(clone_accel) {
    printk(KERN_INFO "clone_accel syscall invoked\n");
    return 0;
}

#endif
