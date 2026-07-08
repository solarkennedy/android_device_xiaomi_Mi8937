#!/vendor/bin/sh
# qmux-flip-on.sh (vendor-resident) — establish the qmux WIN state on a
# pepito-qmux kernel: stock A8 rmt_storage on the legacy IPC Router + rpmsg
# xprt on the modem edge → modem completes RFSA GET_BUFF_ADDR, no
# rmts_get_buffer fatal, full modem QMI surface (incl LOC svc 16).
#
# Runtime-only until cold-boot integration is tuned; re-run after reboot.
# Idempotent. Triggered by `setprop persist.vendor.qmux.enable 1` (see
# init.qmux.rc) or run by hand.
set -e
BIN=/vendor/bin
LIB=/vendor/lib64/qmux

grep -q debugfs /proc/mounts || mount -t debugfs debugfs /sys/kernel/debug

setprop ctl.stop vendor.rmt_storage
sleep 1

if ! pidof qmux_rmt_storage >/dev/null 2>&1; then
	( LD_LIBRARY_PATH="$LIB" "$BIN/qmux_rmt_storage" </dev/null \
		>/data/local/tmp/qmux-rmt.log 2>&1 & )
	sleep 3
fi
pidof qmux_rmt_storage >/dev/null 2>&1 || {
	echo "[!] qmux_rmt_storage failed — /data/local/tmp/qmux-rmt.log"; exit 1; }

echo 1 > /sys/module/ipc_router_rpmsg_xprt/parameters/enable
echo restart > /sys/kernel/debug/msm_subsys/modem
sleep 14

cat /sys/kernel/debug/rmt_storage/info | grep -E "Client_name|Request"
echo "modem: $(cat /sys/bus/msm_subsys/devices/subsys0/state) cc=$(cat /sys/bus/msm_subsys/devices/subsys0/crash_count)"
echo "modem QMI svcs: $(grep -c '0x00000000 |' /sys/kernel/debug/msm_ipc_router/dump_servers 2>/dev/null)"

# Modem now healthy on ipc_router → bring up telephony. qcrild needs
# LD_PRELOAD=libqmi_force_ipcr.so so libqmi_cci uses its native ipc_router
# backend and reaches the modem. Started here, AFTER the modem is ipcr-healthy,
# so qcrild's QMI init doesn't race the flip.
#
# Prefer init (proper radio user + respawn) IF the shipped qcrild.rc carries
# the setenv; otherwise inject the preload here — robust against the vendor
# blob overriding our device-tree qcrild.rc (parallel-extract churn).
if grep -q "libqmi_force_ipcr" /vendor/etc/init/qcrild.rc 2>/dev/null; then
	setprop persist.vendor.radio.autostart 1
	start vendor.qcrild
	echo "qcrild started via init on ipc_router"
else
	echo "[qmux] qcrild.rc lacks LD_PRELOAD; launching qcrild with the shim directly"
	setprop ctl.stop vendor.qcrild 2>/dev/null
	pkill -9 -f qcrild 2>/dev/null
	sleep 1
	setsid env LD_PRELOAD=libqmi_force_ipcr.so /vendor/bin/hw/qcrild </dev/null \
		>/data/local/tmp/qcrild.out 2>&1 &
	echo "qcrild launched with force-ipcr preload"
fi
echo "(SIM/registration via: getprop gsm.sim.state / logcat -b radio)"
