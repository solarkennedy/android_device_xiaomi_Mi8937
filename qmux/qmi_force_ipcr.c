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
// GATED on the qmux props: this lets it be LD_PRELOAD'd UNCONDITIONALLY into
// a service (e.g. the lazy gnss HAL, which init must exec directly — a
// wrapper can't transition into a HAL domain, and a HAL domain can't
// execute_no_trans a non-shell exec_type: hal_neverallows.te). When qmux is
// off the shim is a no-op (real socket()), preserving normal QRTR behavior.
// Gate resolution: persist.vendor.qmux.enable, IF SET, wins (bench override /
// escape hatch); otherwise ro.vendor.qmux.enable decides — set per-variant by
// libinit (1 on pepito, 0 elsewhere), mirroring the kernel xprt's machine
// auto-enable, so pepito ships qmux-on with zero runtime flipping. The props
// are read only on the AF_QIPCRTR probe (rare, at QMI-stack init) — no
// caching, so no stale-value risk.
//
// Scope: preloaded into modem-facing QMI clients (qcrild, gnss HAL). adsp/
// sensor QMI clients that legitimately need QRTR must NOT preload it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/system_properties.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

// Children of a preloaded daemon must NOT inherit LD_PRELOAD: this .so lives
// in /vendor, so any /system helper the daemon execs (netmgrd forks
// ip-wrapper/netutils-wrapper for every address/route/iptables op) dies with
// CANNOT LINK EXECUTABLE -> exit 1 -> netmgrd aborts the data call
// (WDS_CONNECTED then NET_NO_NET). We are already mapped by the time
// constructors run, so scrubbing the env only affects exec'd children.
__attribute__((constructor)) static void qmux_scrub_ld_preload(void)
{
	unsetenv("LD_PRELOAD");
}

static int qmux_enabled(void)
{
	char v[PROP_VALUE_MAX] = {0};

	/* Explicit persist setting wins in both directions. */
	if (__system_property_get("persist.vendor.qmux.enable", v) > 0)
		return strcmp(v, "1") == 0;
	if (__system_property_get("ro.vendor.qmux.enable", v) > 0)
		return strcmp(v, "1") == 0;
	return 0;
}

int socket(int domain, int type, int protocol)
{
	static int (*real_socket)(int, int, int);
	if (!real_socket)
		real_socket = dlsym(RTLD_NEXT, "socket");

	if (domain == AF_QIPCRTR && qmux_enabled()) {
		/* qmux on: pretend no QRTR so QCCI uses its ipcr path. */
		errno = EAFNOSUPPORT;
		return -1;
	}
	return real_socket(domain, type, protocol);
}
