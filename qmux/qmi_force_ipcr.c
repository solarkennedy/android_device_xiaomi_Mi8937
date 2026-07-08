// qmi_force_ipcr.c — [qmux] force QTI libqmi_cci onto its native IPC-Router
// (AF_MSM_IPC) backend instead of QRTR, but ONLY when qmux is enabled.
//
// libqmi_cci picks its transport at runtime via qmi_cci_xprt_qrtr_supported():
//   fd = socket(AF_QIPCRTR, SOCK_DGRAM|SOCK_CLOEXEC, 0);
//   if (fd >= 0)                  -> use QRTR
//   else if (errno==EAFNOSUPPORT) -> use ipcr (AF_MSM_IPC)  <-- what we want
// On this kernel QRTR is present, so the probe succeeds and the client picks
// QRTR — where the pepito A8 modem is absent (it lives on ipc_router, see
// PLAN-qmux.md). This shim fails the AF_QIPCRTR probe so libqmi_cci falls back
// to its (compiled-in) ipcr backend and reaches the modem.
//
// GATED on persist.vendor.qmux.enable == "1": this lets it be LD_PRELOAD'd
// UNCONDITIONALLY into a service (e.g. the lazy gnss HAL, which init must exec
// directly — a wrapper can't transition into a HAL domain, and a HAL domain
// can't execute_no_trans a non-shell exec_type: hal_neverallows.te). When qmux
// is off the shim is a no-op (real socket()), preserving normal QRTR behavior.
// The prop is read only on the AF_QIPCRTR probe (rare, at QMI-stack init, after
// the qmux flip has set it) — no caching, so no stale-value risk.
//
// Scope: preloaded into modem-facing QMI clients (qcrild, gnss HAL). adsp/
// sensor QMI clients that legitimately need QRTR must NOT preload it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/system_properties.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

int socket(int domain, int type, int protocol)
{
	static int (*real_socket)(int, int, int);
	if (!real_socket)
		real_socket = dlsym(RTLD_NEXT, "socket");

	if (domain == AF_QIPCRTR) {
		char v[PROP_VALUE_MAX] = {0};
		if (__system_property_get("persist.vendor.qmux.enable", v) > 0 &&
		    strcmp(v, "1") == 0) {
			/* qmux on: pretend no QRTR so QCCI uses its ipcr path. */
			errno = EAFNOSUPPORT;
			return -1;
		}
	}
	return real_socket(domain, type, protocol);
}
