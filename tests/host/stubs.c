/* No-op replacements for the PS5-only kernel, save-mount and notification
 * APIs, so that main.c can be built and exercised on a Linux host with
 * -DBACKUP_HOST_TEST. Nothing here touches the real system. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

uint64_t kernel_get_ucred_authid(pid_t pid) { (void)pid; return 0; }
int32_t  kernel_set_ucred_authid(pid_t pid, uint64_t authid) { (void)pid; (void)authid; return 0; }
int32_t  kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]) { (void)pid; (void)caps; return 0; }
int32_t  kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]) { (void)pid; (void)caps; return 0; }
intptr_t kernel_get_root_vnode(void) { return 0; }
int32_t  kernel_set_proc_rootdir(pid_t pid, intptr_t vnode) { (void)pid; (void)vnode; return 0; }
intptr_t kernel_get_proc_jaildir(pid_t pid) { (void)pid; return 0; }
int32_t  kernel_set_proc_jaildir(pid_t pid, intptr_t vnode) { (void)pid; (void)vnode; return 0; }

int
klog_printf(const char *fmt, ...) {
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vfprintf(stderr, fmt, ap);
  va_end(ap);
  return n;
}

/* Rich notifications are logged by main.c before they are sent. */
int
sceNotificationSend(int user_id, bool is_logged, const char *payload) {
  (void)user_id; (void)is_logged; (void)payload;
  return 0;
}

int sceKernelGetAppInfo(pid_t pid, void *info) { (void)pid; (void)info; return -1; }

int sceFsInitMountSaveDataOpt(void *opt) { (void)opt; return 0; }
int sceFsMountSaveData(void *opt, const char *path, const char *mount, uint8_t *key) {
  (void)opt; (void)path; (void)mount; (void)key; return -1;
}
int sceFsInitUmountSaveDataOpt(void *opt) { (void)opt; return 0; }
int sceFsUmountSaveData(void *opt, const char *mount, int handle, int ignore) {
  (void)opt; (void)mount; (void)handle; (void)ignore; return -1;
}

/* Settings page: the console browser cannot be opened on the host. */
int sceUserServiceInitialize(void *p) { (void)p; return 0; }
int sceSystemServiceLaunchWebBrowser(const char *uri, void *p) { (void)uri; (void)p; return -1; }
