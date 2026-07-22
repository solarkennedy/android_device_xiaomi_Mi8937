#
# Copyright (C) 2021 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Cryptfshw
TARGET_EXCLUDE_CRYPTFSHW := true

ifeq ($(TARGET_DEVICE_PEPITO),true)
TARGET_USES_DEVICE_SPECIFIC_KEYMASTER := true
endif

# Inherit from mithorium-common
$(call inherit-product, device/xiaomi/mithorium-common/mithorium.mk)

# Sibling Mi8937 devices keep the 2 GB phone heap profile. Pepito uses
# Android Go defaults from lineage_Mi8937.mk instead.
ifneq ($(TARGET_DEVICE_PEPITO),true)
$(call inherit-product, frameworks/native/build/phone-xhdpi-2048-dalvik-heap.mk)
endif

# Boot animation
TARGET_SCREEN_HEIGHT := 1280
TARGET_SCREEN_WIDTH := 720

# Dynamic Partitions
PRODUCT_USE_DYNAMIC_PARTITIONS := false

# Init
$(call soong_config_set,libinit,vendor_init_lib,//$(LOCAL_PATH):init_xiaomi_mi8937)

# Overlays
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay

ifeq ($(PRODUCT_HARDWARE),Mi8917)
PRODUCT_PACKAGES += \
    xiaomi_rolex_overlay \
    xiaomi_riva_overlay \
    xiaomi_ugglite_overlay
else ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_PACKAGES += \
    xiaomi_prada_overlay \
    xiaomi_prada_overlay_Settings \
    xiaomi_ugg_overlay \
    xiaomi_wt8937_overlay \
    xiaomi_wt8937_overlay_Settings

ifeq ($(TARGET_DEVICE_PEPITO),true)
# PepitoLauncher2: stock-Palm-launcher reimplementation, ships alongside
# Trebuchet (not default). Source: packages/apps/PepitoLauncher2, a clone of
# the canonical repo at ~/Projects/PepitoLauncher2 (PLAN-pepitolauncher2.md).
PRODUCT_PACKAGES += \
    PepitoLauncher2 \
    VolumeTile \
    xiaomi_pepito_overlay \
    xiaomi_pepito_overlay_systemui

# Allowlist VolumeTile's signature|privileged STATUS_BAR permission (see the xml).
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/permissions/privapp-permissions-pepito.xml:$(TARGET_COPY_OUT_SYSTEM)/etc/permissions/privapp-permissions-pepito.xml
endif
endif

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.fingerprint.xml:$(TARGET_COPY_OUT_ODM)/etc/permissions/sku_fingerprint/android.hardware.fingerprint.xml

# Audio
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*.xml,$(LOCAL_PATH)/audio/mixer_paths/,$(TARGET_COPY_OUT_VENDOR)/etc/) \
    $(call find-copy-subdir-files,*.xml,$(LOCAL_PATH)/audio/platform_info/,$(TARGET_COPY_OUT_VENDOR)/etc/) \
    $(call find-copy-subdir-files,*,$(LOCAL_PATH)/audio/acdbdata/pepito/,$(TARGET_COPY_OUT_VENDOR)/etc/acdbdata/pepito/)

# pepito speaker smart-amp (NXP TFA9896) DSP tuning container. Installed as the
# tfa98xx driver's default fw_name (tfa98xx.cnt), which it request_firmware()s
# from /vendor/firmware. This is the original Palm PVG100 tuning from stock A8.1.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/tfa/tfa9896.cnt:$(TARGET_COPY_OUT_VENDOR)/firmware/tfa98xx.cnt

# Camera
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/bin/mm-qcamera-daemon \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/etc/camera/.placeholder \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/lib/.placeholder \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_VENDOR)/lib/overlayfs/pepito/libandroidicu.so \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_VENDOR)/lib/overlayfs/pepito/libicu.so \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_VENDOR)/lib/overlayfs/pepito/libicui18n.so \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_VENDOR)/lib/overlayfs/pepito/libicuuc.so

# Disable camera builds — missing legacy headers in Lineage 23.2
# PRODUCT_PACKAGES += \
#     camera.ulysse \
#     camera.wingtech

# ifeq ($(PRODUCT_HARDWARE),Mi8937)
# PRODUCT_PACKAGES += \
#     camera.land
# endif

# Pepito camera HAL — source-built camera.pepito (QCamera2 HAL3). Preview + JPEG
# still-capture verified on both cameras 2026-07-03 (see PLAN-camera.md). Its
# shared_libs (libPmcamera_interface, libPmjpeg_interface, libPomx_core) install
# alongside it to /vendor/odm/lib automatically; the JPEG OMX encoder blobs
# (pepito_libqomx_jpegenc + deps) come from the vendor tree. The provider is
# pointed at it via ro.hardware.camera=pepito (rootdir/etc/init.xiaomi.device.rc).
# The stock AML0 camera.msm8937.so blob (+ libVDBeautyShotAPI + its linker
# fragment) was retired 2026-07-03.
# TODO(layering): gate behind TARGET_DEVICE_PEPITO once wired — sibling Mi8937
# variants select their own camera.<variant>.
PRODUCT_PACKAGES += \
    camera.pepito

# Dumpstate
PRODUCT_PACKAGES += \
    libdumpstate_device

# Filesystem
PRODUCT_PACKAGES += \
    e2fsck_ramdisk \
    tune2fs_ramdisk \
    resize2fs_ramdisk

# Enable project quotas and casefolding for emulated storage without sdcardfs
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# Use FUSE passthrough
PRODUCT_PRODUCT_PROPERTIES += \
    persist.sys.fuse.passthrough.enable=true

ifeq ($(TARGET_DEVICE_PEPITO),true)
# Pepito's stock QTI keymaster wrapper rejects Android 16 OS version tags during
# configure. The blob is patched to read these stock-compatible values instead
# of ro.build.version.* for the configure command. Vendor props (they configure
# a vendor blob); labeled vendor_pepito_keymaster_prop for hal_keymaster_qti.
PRODUCT_VENDOR_PROPERTIES += \
    ro.keymaster.xxx.release=8.1.0 \
    ro.keymaster.xxx.security_patch=2020-09-01

# Compressed hardware offload hard-fails on this ADSP: the DSP rejects
# ASM_STREAM_CMD_OPEN_WRITE_V3 with ADSP_EFAILED, so any app that requests
# offload (e.g. Twelve, which defaults enableOffload=true) sees AudioTrack
# ERROR_DEAD_OBJECT and dies. Force all playback onto the working PCM path.
# Verified live: setprop audio.offload.disable 1 + audioserver restart made
# Twelve play the low-latency-playback usecase with no errors.
# NOTE: this does NOT make the speaker audible on its own — pepito's speaker is
# driven by an external NXP TFA9896 smart-amp that is not yet brought up in this
# build (see PLAN-audio.md "External speaker amp (TFA9896)").
PRODUCT_SYSTEM_PROPERTIES += \
    audio.offload.disable=1

# OTA: point the Updater app (packages/apps/Updater) at our own static feed
# instead of a real LineageOS OTA server. Builds are hosted as GitHub Release
# assets on solarkennedy/lineageos-pepito; the JSON below is generated per
# release by lineageos-pepito/scripts/gen-ota-json.py. No {device}/{type}/{incr}
# placeholders needed since the file already covers only this device.
PRODUCT_SYSTEM_PROPERTIES += \
    lineage.updater.uri=https://raw.githubusercontent.com/solarkennedy/lineageos-pepito/lineageos23.2/pepito.json
endif

# Face unlock — Phase 2: Paranoid Sense port (packages/apps/FaceUnlock, crDroid
# 16.0). Real RGB recognition via Megvii engine (Moto blobs, arm64), app-level
# (co.aospa.sense on system_ext) bridged into FaceService by the SenseProvider
# frameworks/base patch, gated on ro.face.sense_service. Registers as a WEAK
# face sensor; enrollment redirected to the app's EnrollActivity by
# FaceUnlockOverlay's config_face_enroll. Phase 1 (AOSP virtual IFace HAL)
# retired 2026-07-12 after validating the framework path end-to-end; the
# feature XML below is still required to advertise FEATURE_FACE.
# See PLAN-face-unlock.md.
ifeq ($(TARGET_DEVICE_PEPITO),true)
PRODUCT_PACKAGES += \
    FaceUnlock

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.biometrics.face.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.biometrics.face.xml

PRODUCT_SYSTEM_PROPERTIES += \
    ro.face.sense_service=true
endif

# Fingerprint
ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/bin/gx_fpd

PRODUCT_PACKAGES += \
    android.hardware.biometrics.fingerprint@2.1-service.xiaomi_ulysse \
    android.hardware.biometrics.fingerprint@2.1-service.xiaomi_wt8937

PRODUCT_PACKAGES += \
    liblzma.vendor:64
endif

# Input
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(LOCAL_PATH)/keylayout/,$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/) \
    $(foreach f, msm8917-sku5-snd-card_Button_Jack.kl msm8920-sku7-snd-card_Button_Jack.kl msm8952-sku1-snd-card_Button_Jack.kl, \
        $(LOCAL_PATH)/keylayout/msm8952-snd-card-mtp_Button_Jack.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/$(f))

# LiveDisplay
$(call soong_config_set_bool,livedisplay_sysfs,enable_re,true)

# Placeholder
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/bin/.placeholder \
    $(LOCAL_PATH)/configs/blankfile:$(TARGET_COPY_OUT_ODM)/lib64/.placeholder

# Power
$(call soong_config_set,qtipower,tap_to_wake_node,/proc/sys/dev/xiaomi_msm8937_touchscreen/enable_dt2w)

# Recovery
ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_COPY_FILES += \
    vendor/xiaomi/Mi8937/proprietary/vendor/bin/hvdcp_opti:$(TARGET_COPY_OUT_RECOVERY)/root/system/bin/hvdcp_opti
endif

# Rootdir
PRODUCT_PACKAGES += \
    fstab.qcom.ramdisk \
    init.baseband.sh \
    init.xiaomi.device.rc \
    init.xiaomi.device.sh

ifeq ($(TARGET_DEVICE_PEPITO),true)
# Stock QTI keymaster HIDL wrapper (property-spoofed configure; see
# ro.keymaster.xxx.* above). The QSEE keymaster TA itself is APPSBL/TZ-preloaded
# as keymaster64 and aliased in the kernel qseecom driver — no trustlet files
# are loaded from the filesystem, so no firmware staging/mirror is needed.
PRODUCT_PACKAGES += \
    android.hardware.keymaster@3.0-service-qti \
    android.hardware.keymaster@3.0-impl-qti \
    android.hardware.keymaster@3.0.vendor \
    libkeymasterdeviceutils \
    libkeymasterutils
endif

ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_PACKAGES += \
    init.goodix.sh
endif

# Shims
PRODUCT_PACKAGES += \
    libshims_android \
    libshims_ui \
    libwui

ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_PACKAGES += \
    libbinder_shim.vendor \
    libc_mutexdestroy_shim \
    libc_pthreadts_shim \
    libfakelogprint

PRODUCT_COPY_FILES += \
    prebuilts/vndk/v32/arm64/arch-arm64-armv8-a/shared/vndk-sp/libhidlbase.so:$(TARGET_COPY_OUT_ODM)/lib64/libhidlbase-v32.so
endif

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH)

# Wifi
ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_PACKAGES += \
    WifiOverlay_prada
endif

# Inherit from vendor blobs
ifeq ($(PRODUCT_HARDWARE),Mi8917)
$(call inherit-product, vendor/xiaomi/Mi8917/Mi8917-vendor.mk)
else ifeq ($(PRODUCT_HARDWARE),Mi8937)
$(call inherit-product, vendor/xiaomi/Mi8937/Mi8937-vendor.mk)
endif

# Pepito-specific hals.conf adds sensors.native.so (BST BHy HAL) alongside the
# SSC sub-HAL.  This overrides the mithorium-common base which only has sensors.ssc.so.
# sensors.native.so is only installed for this build (Mi8937-vendor.mk), so other
# variants picking up this file would get a harmless "not found" warning from multihal.
PRODUCT_COPY_FILES += \
    device/xiaomi/Mi8937/configs/sensors/hals.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/hals.conf

# qmux legacy-IPC modem path (pepito) — stock A8 rmt_storage + its private A8
# vendor-lib closure (isolated under lib64/qmux, loaded via LD_LIBRARY_PATH so
# they never shadow the A15 vendor libs) and the static init selection
# (init.qmux.rc, keyed on the libinit-set ro.vendor.qmux.enable; on for
# pepito, off elsewhere). No flip script: the kernel xprt auto-enables on
# pepito, so boot lands in the qmux world directly. See PLAN-qmux.md /
# PLAN-qmux-bridge.md.
ifeq ($(PRODUCT_HARDWARE),Mi8937)
PRODUCT_COPY_FILES += \
    device/xiaomi/Mi8937/qmux/bin/qmux_rmt_storage:$(TARGET_COPY_OUT_VENDOR)/bin/qmux_rmt_storage \
    device/xiaomi/Mi8937/qmux/init.qmux.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.qmux.rc \
    device/xiaomi/Mi8937/qmux/lib64/libCheckTunning.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libCheckTunning.so \
    device/xiaomi/Mi8937/qmux/lib64/libJrdQmi.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libJrdQmi.so \
    device/xiaomi/Mi8937/qmux/lib64/libbackuptunning.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libbackuptunning.so \
    device/xiaomi/Mi8937/qmux/lib64/libdiag.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libdiag.so \
    device/xiaomi/Mi8937/qmux/lib64/libdsutils.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libdsutils.so \
    device/xiaomi/Mi8937/qmux/lib64/libidl.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libidl.so \
    device/xiaomi/Mi8937/qmux/lib64/libmdmdetect.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libmdmdetect.so \
    device/xiaomi/Mi8937/qmux/lib64/libqmi_cci.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libqmi_cci.so \
    device/xiaomi/Mi8937/qmux/lib64/libqmi_client_qmux.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libqmi_client_qmux.so \
    device/xiaomi/Mi8937/qmux/lib64/libqmi_common_so.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libqmi_common_so.so \
    device/xiaomi/Mi8937/qmux/lib64/libqmi_csi.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libqmi_csi.so \
    device/xiaomi/Mi8937/qmux/lib64/libqmi_encdec.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libqmi_encdec.so \
    device/xiaomi/Mi8937/qmux/lib64/libqmiservices.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libqmiservices.so \
    device/xiaomi/Mi8937/qmux/lib64/libsmemlog.so:$(TARGET_COPY_OUT_VENDOR)/lib64/qmux/libsmemlog.so \
    device/xiaomi/Mi8937/qmux/qcrild.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/qcrild.rc

# VoLTE NV self-heal oneshot (started by init.qmux.rc; see PLAN-volte.md)
PRODUCT_PACKAGES += \
    ims_enabler
# NOTE: libqmi_force_ipcr (the force-ipcr shim) is defined in mithorium-common
# libshim/ and shipped via its gps_vendor_product.mk (the gnss service links
# it; the qmux blobs carry it as a patched-in DT_NEEDED). Not duplicated here.
endif
