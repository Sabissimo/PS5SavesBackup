/* Host-test stand-in for the PS5 Payload SDK header. Same prototypes,
 * implemented as no-ops in tests/host/stubs.c. */
#ifndef HOST_PS5_KERNEL_H
#define HOST_PS5_KERNEL_H
#include <stdint.h>
#include <sys/types.h>

uint64_t kernel_get_ucred_authid(pid_t pid);
int32_t  kernel_set_ucred_authid(pid_t pid, uint64_t authid);
int32_t  kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
int32_t  kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);
intptr_t kernel_get_root_vnode(void);
int32_t  kernel_set_proc_rootdir(pid_t pid, intptr_t vnode);
intptr_t kernel_get_proc_jaildir(pid_t pid);
int32_t  kernel_set_proc_jaildir(pid_t pid, intptr_t vnode);

#endif
