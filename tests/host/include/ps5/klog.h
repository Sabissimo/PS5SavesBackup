/* Host-test stand-in for the PS5 Payload SDK header: klog goes to stderr. */
#ifndef HOST_PS5_KLOG_H
#define HOST_PS5_KLOG_H

int klog_printf(const char *fmt, ...);

#endif
