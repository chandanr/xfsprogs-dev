#ifndef __KERNEL_TYPES_H__
#define __KERNEL_TYPES_H__

typedef __u8	u8;
typedef __u16	u16;
typedef __u32	u32;
typedef __u64	u64;

typedef struct {
	uid_t val;
} kuid_t;

typedef struct {
	gid_t val;
} kgid_t;

#endif	/* __KERNEL_TYPES_H__ */
