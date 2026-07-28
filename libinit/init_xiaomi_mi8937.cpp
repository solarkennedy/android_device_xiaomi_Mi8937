/*
 * Copyright (C) 2021 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libinit_dalvik_heap.h>
#include <libinit_utils.h>
#include <libinit_variant.h>

#include "vendor_init.h"

#include <android-base/file.h>
#include <android-base/properties.h>
#include <fstab/fstab.h>

static const variant_info_t ugglite_info = {
    .brand = "xiaomi",
    .device = "ugglite",
    .marketname = "",
    .model = "Redmi Note 5A",
    .build_fingerprint = "",
    .dpi = 260,
};

static const variant_info_t ugg_info = {
    .brand = "xiaomi",
    .device = "ugg",
    .marketname = "",
    .model = "Redmi Note 5A",
    .build_fingerprint = "",
    .dpi = 260,
};

static const variant_info_t rolex_info = {
    .brand = "Xiaomi",
    .device = "rolex",
    .marketname = "",
    .model = "Redmi 4A",
    .build_fingerprint = "",
    .dpi = 280,
};

static const variant_info_t riva_info = {
    .brand = "Xiaomi",
    .device = "riva",
    .marketname = "",
    .model = "Redmi 5A",
    .build_fingerprint = "",
    .dpi = 280,
};

static const variant_info_t land_info = {
    .brand = "Xiaomi",
    .device = "land",
    .marketname = "",
    .model = "Redmi 3S",
    .build_fingerprint = "",
    .dpi = 280,
};

static const variant_info_t santoni_info = {
    .brand = "Xiaomi",
    .device = "santoni",
    .marketname = "",
    .model = "Redmi 4X",
    .build_fingerprint = "",
    .dpi = 280,
};

static const variant_info_t prada_info = {
    .brand = "Xiaomi",
    .device = "prada",
    .marketname = "",
    .model = "Redmi 4",
    .build_fingerprint = "",
    .dpi = 280,
};

static const variant_info_t pepito_info = {
    .brand = "Palm",
    .device = "pepito",
    .marketname = "",
    .model = "PVG100",
    .build_fingerprint = "",
    .dpi = 264,
};

static void replace_all(std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty())
        return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
}

// Normalize the public device identity from the Mi8937-common BUILD codename to
// the real Palm PVG100/pepito. The fingerprint's product/device fields come
// from TARGET_PRODUCT (lineage_Mi8937[_gapps]) and TARGET_DEVICE (Mi8937),
// which also name the build output dir / lunch combos and so can't be renamed
// cheaply -- but the runtime props can be. Derive the corrected fingerprint
// from the LIVE one so every dynamic field (version, build id, incremental,
// security patch, variant, tags) stays truthful; only the codename tokens
// change. No-op if the fingerprint is unreadable or already clean, so this can
// never wedge boot. ro.product.name is fixed to match (stock reports PVG100).
static void set_pepito_identity()
{
    std::string fp = android::base::GetProperty("ro.build.fingerprint", "");
    if (fp.empty() || fp.find("Mi8937") == std::string::npos)
        return;

    // Replace the product token(s) first (they embed "Mi8937"), then the bare
    // device token, so the device pass can't corrupt the product field. The
    // product field is TARGET_PRODUCT: lineage_Mi8937_gapps (gapps) or
    // lineage_Mi8937 (vanilla) -- match the longer one first. NB: matched as a
    // literal rather than read from ro.product.name, because the aggregate
    // ro.product.name is NOT populated yet at vendor_load_properties time (init
    // assembles it later from the per-partition props); only static props like
    // ro.build.fingerprint are readable this early.
    replace_all(fp, "lineage_Mi8937_gapps", "PVG100");
    replace_all(fp, "lineage_Mi8937", "PVG100");
    replace_all(fp, "Mi8937", "pepito");

    set_ro_build_prop("fingerprint", fp);
    property_override("ro.bootimage.build.fingerprint", fp);
    property_override("ro.build.description", fingerprint_to_description(fp));
    set_ro_build_prop("name", "PVG100", true);
}

static void determine_device()
{
    std::string codename;

    // qmux: pepito's A8 modem lives on the legacy ipc_router, and the kernel
    // (ipc_router_rpmsg_xprt auto-enable) puts the modem edge there at cold
    // boot. This prop is the userspace half of the same switch: init.qmux.rc
    // keys the rmt_storage/qcrild/netmgrd/IMS service selection off it, and
    // the libqmi_force_ipcr preload consults it in every QMI client that
    // carries it. Default off; flipped on for pepito below.
    property_override("ro.vendor.qmux.enable", "0");

    android::base::ReadFileToString("/sys/xiaomi-msm8937-mach/codename", &codename, true);
    if (codename.empty())
        return;
    codename.pop_back();

    if (codename == "rolex") {
        set_variant_props(rolex_info);
    } else if (codename == "riva") {
        set_variant_props(riva_info);
    } else if (codename == "land") {
        set_variant_props(land_info);
        goto read_wingtech_board_id;
    } else if (codename == "santoni") {
        set_variant_props(santoni_info);
        goto read_wingtech_board_id;
    } else if (codename == "ugglite") {
        set_variant_props(ugglite_info);
    } else if (codename == "prada") {
        set_variant_props(prada_info);
    } else if (codename == "pepito") {
        set_variant_props(pepito_info);
        property_override("ro.vendor.qmux.enable", "1");
        set_pepito_identity();
    } else if (codename == "ugg") {
        set_variant_props(ugg_info);
    }

    return;

read_wingtech_board_id:
    std::string wingtech_board_id;

    android::base::ReadFileToString("/sys/xiaomi-msm8937-mach/wingtech_board_id", &wingtech_board_id, true);
    if (wingtech_board_id.empty())
        return;
    wingtech_board_id.pop_back();

    if (codename == "land" && wingtech_board_id == "S88537AB1") {
        set_ro_build_prop("model", "Redmi 3X", true);
    } else if (codename == "santoni" && wingtech_board_id == "S88536CA2") {
        set_ro_build_prop("model", "Redmi 4", true);
    }

    return;
}

static void enable_gatekeeper_uid_offset() {
    std::string boot_device = *android::fs_mgr::GetBootDevices().begin();
    if (boot_device == "soc/7864900.sdhci") {
        property_override("ro.gsid.image_running", "1");
    }
}

void vendor_load_properties() {
    determine_device();
    enable_gatekeeper_uid_offset();
    set_bootloader_prop();
    set_dalvik_heap();
}
