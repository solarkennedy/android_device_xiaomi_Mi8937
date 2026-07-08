#!/vendor/bin/sh
# qmux-flip-on.sh (vendor-resident) — establish the qmux WIN state on a
# pepito-qmux kernel: stock A8 rmt_storage on the legacy IPC Router + rpmsg
# xprt on the modem edge → modem completes RFSA GET_BUFF_ADDR, no
# rmts_get_buffer fatal, full modem QMI surface (incl LOC svc 16) → qcrild
# on ipc_router → telephony.
#
# Run via init.qmux.rc (exec_background) on persist.vendor.qmux.enable=1, or by
# hand. The long-running daemons are proper init SERVICES (qmux_rmt_storage,
# qmux_qcrild in init.qmux.rc) started here — NOT backgrounded, because init
# SIGKILLs an exec_background script's process group on exit, which would kill
# any daemon this script forked. Idempotent.
set -e

grep -q debugfs /proc/mounts || mount -t debugfs debugfs /sys/kernel/debug

# 1. Swap rmt_storage: nightly (QRTR) → stock A8 (ipc_router), as init services.
setprop ctl.stop vendor.rmt_storage
sleep 1
start qmux_rmt_storage
sleep 3
pidof qmux_rmt_storage >/dev/null 2>&1 || {
	echo "[!] qmux_rmt_storage failed — /data/local/tmp/qmux-rmt.log"; exit 1; }

# 2. Flip the modem edge to the legacy router and re-init the modem on it.
echo 1 > /sys/module/ipc_router_rpmsg_xprt/parameters/enable
echo restart > /sys/kernel/debug/msm_subsys/modem
sleep 14

cat /sys/kernel/debug/rmt_storage/info | grep -E "Client_name|Request"
echo "modem: $(cat /sys/bus/msm_subsys/devices/subsys0/state) cc=$(cat /sys/bus/msm_subsys/devices/subsys0/crash_count)"
echo "modem QMI svcs: $(grep -c '0x00000000 |' /sys/kernel/debug/msm_ipc_router/dump_servers 2>/dev/null)"

# 3. Telephony: qmux_qcrild carries the force-ipcr preload (init.qmux.rc), so
# libqmi_cci uses its native ipc_router backend. Started AFTER the modem is
# ipcr-healthy so qcrild's QMI init doesn't race the flip.
start qmux_qcrild
echo "qmux_qcrild started on ipc_router (SIM/registration via: getprop gsm.sim.state)"

# GPS: the gnss HAL is repointed to /vendor/bin/gnss-qmux-wrapper.sh
# (mithorium-common gnss rc), which preloads the same shim when qmux is enabled
# so its loc_api_v02/libqmi_cci binds QMI_LOC (svc 16) on ipc_router. No action
# needed here; the HAL is lazy-started by the location framework.
