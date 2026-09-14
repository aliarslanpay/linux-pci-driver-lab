// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
static int __init edu_lab_init(void) { return 0; }
static void __exit edu_lab_exit(void) { }
module_init(edu_lab_init);
module_exit(edu_lab_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QEMU EDU personal driver lab");
