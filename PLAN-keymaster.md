# Keymaster / Keystore2 Boot Investigation

## Symptom (original)

Device hung at boot with `vdc keymaster earlyBootEnded` blocked indefinitely.
`android.security.maintenance` (provided by keystore2) never registered.
`vendor.keymaster-3-0` crashed or hung in a loop.

## Current state

Boot now reaches zygote with **hardware-backed keymaster active**. Software
gatekeeper used as a temporary fallback because `vendor.gatekeeper-1-0`
SIGABRTs (see "Open: gatekeeper SIGABRT" below). The pre-existing
surfaceflinger and sensors.qti issues are unchanged.

## Root cause chain (resolved)

```
vdc keymaster earlyBootEnded
  → needs android.security.maintenance (AIDL from keystore2)
  → keystore2 blocks waiting for keymaster HIDL to respond
  → keymaster HIDL never registers because keymaster2_open never returns
  → keymaster2_open enters TZ via /dev/qseecom and waits for a listener
    callback (RPMB / SSD / etc.) that nothing in userspace is answering
  → because vendor.qseecomd is not running
  → because qseecomd was either disabled (Phase A) or had missing
    transitive shared-library dependencies (Phase B)
```

## What's on the device (TZ side, all verified)

- **Per-TA partitions**: `keymaster` / `keymasterbak` (mmcblk0p46/47),
  `cmnlib` / `cmnlib64`, `tz` / `tzbak`. The `keymaster` partition is a
  1 MB ELF whose strings include `KEYMASTER Init`,
  `KM_ERROR_KEYMASTER_NOT_CONFIGURED`, `qsee_SW_GENERIC_ECC_*`.
- **No separate gatekeeper TA partition.** `gatekeeper.msm8937.so`
  shares the keymaster TA — its strings include `"Loading keymaster app
  failed"`, `"gatekeeper"`, `"keymaster"`. The Qualcomm keymaster
  trustlet implements both keymaster and gatekeeper functions.
- **TZBSP** has RPMB rollback protection active
  (`tzbsp application rpmb version rollback label`), so the TA *will*
  callback into Linux through the RPMB listener.
- **QSEECOM kernel driver** initializes each boot
  (`qsee_version = 0x1000000`); `/dev/qseecom` is present and correctly
  labeled `tee_device`.

The TZ stack is fully provisioned. Every problem we hit was userspace.

## What qseecomd does and why it's required

`/vendor/bin/init.qti.qseecomd.sh` is a barrier — it polls
`vendor.sys.listeners.registered` and blocks `on post-fs` until true.
The `qseecomd` daemon registers TZ listener services (RPMB, SSD,
secureui, GP-req-cancel, QISL, drmfs, drmtime, ADSP) and sets that
property when they're all up.

The keymaster TA makes synchronous callbacks through those listeners
(RPMB for monotonic counters used in rollback protection, SSD for secure
storage of the master key blob). Without listeners registered, the first
such callback blocks the entire `keymaster2_open` syscall path.

## Two false leads (so you don't repeat them)

1. **"qseecomd is missing from the nightly"** — the prior session ran
   `ls /mnt/vendor-nightly/bin/` as a non-root user, which silently
   returns nothing on a mode-710 directory. `sudo ls` shows qseecomd,
   init.qti.qseecomd.sh, and the keymaster HIDL service binary all
   present. Always sudo when inspecting Android vendor.img mounts.

2. **"linker says libQSEEComAPI.so not found"** — recovery's linker
   namespace isn't the same as boot. Running a vendor binary directly
   from a recovery shell will print misleading "not found" errors for
   libs the boot-time vendor namespace would resolve fine. To test the
   *real* search path, mount vendor at `/vendor` (not `/v` or `/tmp/x`)
   so the linker recognizes it as a vendor binary. Even then, recovery
   lacks the full VNDK setup so errors past the first resolution layer
   are still artifacts.

## The actual root cause of the qseecomd status-1 exit

`qseecomd` has a transitive shared-library closure that extends well
beyond its `DT_NEEDED`. We extracted the first level
(libQSEEComAPI, libdrmfs, libdrmtime, librpmb, libssd, libsecureui,
libsecureui_svcsock, libGPreqcancel, libGPreqcancel_svc, libqisl) but
those libs themselves depend on libs we didn't have:

- `libdiag.so` — needed by libdrmfs, libdrmtime, libssd, libGPreqcancel
- `libtime_genoff.so` — needed by libdrmtime
- `libStDrvInt.so` — needed by libsecureui
- `vendor.qti.hardware.tui_comm@1.0.so` — needed by libsecureui_svcsock

(Plus `libxml2`, `libdisplayconfig.qti`, `vendor.display.config@1.0/@2.0`
which are also referenced, but those are built from source in the tree
so we don't extract prebuilts — `libxml2` is `vendor_available: true`
in AOSP, and the display-config family is in
`vendor/qcom/opensource/display/services` and `interfaces/display/config`.)

### How to find the full closure

Walk the readelf `NEEDED` graph until stable:

```bash
for lib in $(your-extracted-set); do
  readelf -d "$lib" | grep NEEDED | awk '{print $5}' | tr -d '[]'
done | sort -u
```

Any name in the output that isn't a standard system lib (libc, libm,
libdl, libcutils, libutils, liblog, libc++, libbase, libhidlbase,
libbinder, libhardware) and isn't in `external/` or `vendor/qcom/...`
needs to be extracted.

## The fix (executed)

1. Pulled qseecomd, init.qti.qseecomd.sh, and the full listener-lib
   closure from `/mnt/vendor-nightly` into
   `vendor/xiaomi/Mi8937/proprietary/`. **64-bit only** — there is no
   32-bit qseecomd or 32-bit listener libs in the nightly, because the
   TA is 64-bit.
2. Added the entries to `device/xiaomi/Mi8937/proprietary-files.txt`
   and regenerated `Mi8937-vendor.mk` + `Android.bp` with
   `PYTHONPATH=../../../tools/extract-utils python3 extract-files.py -n -m /mnt/vendor-nightly`.
3. In `mithorium-common/rootdir/etc/init.target.rc`: removed `disabled`
   from `service vendor.qseecomd`, restored `start vendor.qseecomd` and
   `exec - system system -- /vendor/bin/init.qti.qseecomd.sh` under
   `on post-fs`.
4. `init.qti.qseecomd.sh` itself comes from
   `mithorium-common/rootdir/bin/` (bounded 10-second timeout loop) —
   the unbounded nightly version is not extracted, to avoid the
   packaging conflict and because the bounded version is safer.
5. Hardware gatekeeper (`gatekeeper@1.0-impl`/`-service`) was briefly
   enabled — caused SIGABRT in zygote startup — reverted to
   `gatekeeper@1.0-service.software`. See open issue below.

## Status of the KeymasterDevice.cpp software-fallback patch

The patch in `hardware/interfaces/keymaster/3.0/default/KeymasterDevice.cpp`
replaces all hardware-open error paths with a software fallback instead
of returning `nullptr`. With qseecomd now working, the patch is a no-op
when hardware works.

**Recommend reverting**, with the same reasoning as before: silent
fallback hides hardware failures, invalidates hardware-bound keys
(different KEK derivation), breaks attestation. Leaving the patch in
makes a hardware regression invisible. Pending user decision.

## Open: gatekeeper SIGABRT

`vendor.gatekeeper-1-0` (the `gatekeeper@1.0-impl`/`-service` pair)
crashes with SIGABRT immediately on startup. Cascades into zygote and
system_server crash loops because the framework can't function without
gatekeeper.

What we know:
- `gatekeeper.msm8937.so` is present and links cleanly.
- It shares the keymaster TA (no separate gatekeeper partition exists).
- The keymaster TA itself is working (keymaster HIDL responds, no more
  earlyBootEnded hang).
- SIGABRT (not exit-1) means something is asserting/aborting, not a
  clean error return — `defaultPassthroughServiceImplementation`
  would exit-1 on a null device pointer.

What we don't know:
- Whether it's a SELinux denial, a keymaster-app load race, an
  assertion inside the HIDL service, or something else.
- adb isn't reliable because zygote-restart cycles tear it down.

To debug: add `stdio_to_kmsg` to the `vendor.gatekeeper-1-0` service
in its init.rc, rebuild vendor.img, reflash, capture serial output of
the actual abort message. Or grab a tombstone from `/data/tombstones/`
once the device boots far enough to write one.

## FBE on /data

Disabled for performance — `fstab.qcom` has `fileencryption=`,
`metadata_encryption=`, `inlinecrypt`, `keydirectory=` removed from
`/data` entries. `checkpoint=fs` retained. Independent of the keymaster
work.

## Files changed (cumulative)

| File | Change |
|------|--------|
| `device/xiaomi/Mi8937/proprietary-files.txt` | Added qseecomd + listener libs + transitive deps |
| `vendor/xiaomi/Mi8937/proprietary/vendor/bin/qseecomd` | New, from nightly |
| `vendor/xiaomi/Mi8937/proprietary/vendor/lib64/lib{QSEEComAPI,drmfs,drmtime,rpmb,ssd,secureui,secureui_svcsock,GPreqcancel,GPreqcancel_svc,qisl,diag,time_genoff,StDrvInt}.so` | New, from nightly |
| `vendor/xiaomi/Mi8937/proprietary/vendor/lib64/vendor.qti.hardware.tui_comm@1.0.so` | New, from nightly |
| `vendor/xiaomi/Mi8937/Mi8937-vendor.mk` | Regenerated |
| `vendor/xiaomi/Mi8937/Android.bp` | Regenerated |
| `device/xiaomi/mithorium-common/rootdir/etc/init.target.rc` | qseecomd service re-enabled (`disabled` removed); `start` + `exec` wrapper restored in `on post-fs` |
| `device/xiaomi/mithorium-common/mithorium.mk` | Gatekeeper kept on `*-service.software` (hardware path documented in comment, reverted pending SIGABRT diagnosis) |
| `device/xiaomi/Mi8937/rootdir/etc/fstab.qcom` | FBE disabled on /data |
| `hardware/interfaces/keymaster/3.0/default/KeymasterDevice.cpp` | Software-fallback patch — pending decision (recommend revert) |
