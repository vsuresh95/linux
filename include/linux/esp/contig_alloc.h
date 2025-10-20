#ifndef __LINUX_VIRTACC_CONTIG_ALLOC_H
#define __LINUX_VIRTACC_CONTIG_ALLOC_H

#ifdef CONFIG_VIRTUAL_ACCELERATORS

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/ioctl.h>

#define CONTIG_MAJOR 240
#define CONTIG_MINOR 0

extern int contig_init(void);
extern void contig_exit(void);

#else

inline int contig_init(void) { return -ENODEV; }
inline void contig_exit(void) {}

#endif /* CONFIG_VIRTUAL_ACCELERATORS */

#endif
