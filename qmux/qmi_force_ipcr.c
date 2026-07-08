// qmi_force_ipcr.c — [qmux bridge, Architecture D] force QTI libqmi_cci onto
// its native IPC-Router (AF_MSM_IPC) backend instead of QRTR.
//
// libqmi_cci selects its transport at runtime via qmi_cci_xprt_qrtr_supported():
//   fd = socket(AF_QIPCRTR, SOCK_DGRAM|SOCK_CLOEXEC, 0);
//   if (fd >= 0)            -> use QRTR
//   else if (errno==EAFNOSUPPORT) -> use ipcr (AF_MSM_IPC)  <-- what we want
//   else                   -> use QRTR
// On this kernel QRTR IS present (adsp/sensors need it), so the probe succeeds
// and qcrild picks QRTR — where the pepito A8 modem is absent. Our modem lives
// on ipc_router (see PLAN-qmux.md). This shim makes ONLY the AF_QIPCRTR probe
// fail with EAFNOSUPPORT inside the preloaded process, so libqmi_cci falls back
// to its (fully compiled-in) ipcr backend and reaches the modem natively.
//
// Scope it per-service (LD_PRELOAD on qcrild / loc HAL only) — never
// system-wide: adsp/sensor QMI clients must keep real QRTR.
//
// Build (NDK/AOSP clang, aarch64):
//   clang --target=aarch64-linux-android31 -shared -fPIC -O2 \
//         qmi_force_ipcr.c -o libqmi_force_ipcr.so -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <sys/socket.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

int socket(int domain, int type, int protocol)
{
	static int (*real_socket)(int, int, int);
	if (!real_socket)
		real_socket = dlsym(RTLD_NEXT, "socket");

	if (domain == AF_QIPCRTR) {
		/* Pretend the kernel has no QRTR so QCCI uses its ipcr path. */
		errno = EAFNOSUPPORT;
		return -1;
	}
	return real_socket(domain, type, protocol);
}
