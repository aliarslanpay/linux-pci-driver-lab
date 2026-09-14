/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef EDU_LAB_H
#define EDU_LAB_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define EDU_LAB_ABI_VERSION 1U
#define EDU_LAB_DMA_BYTES 4096U
#define EDU_LAB_MAX_FACTORIAL 12U
#define EDU_LAB_FEATURE_COMPUTE 1U
#define EDU_LAB_FEATURE_DMA 2U
#define EDU_LAB_FEATURE_IRQ 4U
enum edu_lab_state {
    EDU_LAB_IDLE, EDU_LAB_COMPUTE, EDU_LAB_DMA_TO,
    EDU_LAB_DMA_FROM, EDU_LAB_FAILED, EDU_LAB_REMOVED
};
struct edu_lab_header {
    __u32 version;
    __u32 size;
    __u32 flags;
    __u32 reserved;
};
struct edu_lab_caps {
    struct edu_lab_header header;
    __u32 device_id;
    __u32 dma_bytes;
    __u32 dma_bits;
    __u32 max_factorial;
    __u32 timeout_ms;
    __u32 state;
    __u32 features;
    __u32 reserved;
    __aligned_u64 computations;
    __aligned_u64 dma_loopbacks;
    __aligned_u64 interrupts;
    __aligned_u64 timeouts;
};
struct edu_lab_compute {
    struct edu_lab_header header;
    __u32 input;
    __u32 result;
    __u32 reserved[2];
};
struct edu_lab_dma {
    struct edu_lab_header header;
    __u32 offset;
    __u32 length;
    __u32 reserved[2];
    __u8 data[EDU_LAB_DMA_BYTES];
};
#define EDU_LAB_IOC_MAGIC 'E'
#define EDU_LAB_GET_CAPS _IOWR(EDU_LAB_IOC_MAGIC, 0, struct edu_lab_caps)
#define EDU_LAB_COMPUTE _IOWR(EDU_LAB_IOC_MAGIC, 1, struct edu_lab_compute)
#define EDU_LAB_DMA_LOOPBACK _IOWR(EDU_LAB_IOC_MAGIC, 2, struct edu_lab_dma)
#endif
