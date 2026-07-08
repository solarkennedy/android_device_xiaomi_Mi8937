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
