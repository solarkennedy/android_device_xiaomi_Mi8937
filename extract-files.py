#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

from extract_utils.file import File
from extract_utils.fixups_blob import (
    BlobFixupCtx,
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

namespace_imports = [
    'device/xiaomi/mithorium-common',
]

blob_fixups: blob_fixups_user_type = {
    'vendor/lib64/hw/android.hardware.keymaster@3.0-impl-qti.so': blob_fixup()
        .binary_regex_replace(
            rb'ro\.build\.version\.release\x00',
            b'ro.keymaster.xxx.release\x00',
        )
        .binary_regex_replace(
            rb'ro\.build\.version\.security_patch\x00',
            b'ro.keymaster.xxx.security_patch\x00',
        ),
    # [qmux] The force-ipcr shim rides as DT_NEEDED, not LD_PRELOAD: init may
    # never grant noatsecure (neverallow, b/140789528), so under Enforcing
    # AT_SECURE=1 makes bionic silently scrub LD_PRELOAD. The shim self-gates
    # on the qmux props (no-op for non-pepito variants).
    'vendor/bin/hw/qcrild': blob_fixup().add_needed('libqmi_force_ipcr.so'),
    'vendor/bin/netmgrd': blob_fixup().add_needed('libqmi_force_ipcr.so'),
    'vendor/bin/imsqmidaemon': blob_fixup().add_needed('libqmi_force_ipcr.so'),
    'vendor/bin/imsdatadaemon': blob_fixup().add_needed('libqmi_force_ipcr.so'),
    'vendor/bin/ims_rtp_daemon': blob_fixup().add_needed('libqmi_force_ipcr.so'),
}

module = ExtractUtilsModule(
    'Mi8937',
    'xiaomi',
    namespace_imports=namespace_imports,
    add_firmware_proprietary_file=True,
    blob_fixups=blob_fixups,
)

# The new extract-utils only auto-registers proprietary-files.txt; the extra
# lists must be added explicitly or their entries silently vanish from the
# generated makefiles on regen (this dropped all 505 camera blobs from the
# 2026-07-10 build: mm-camera sensor init failed with no chromatix → 0×0
# stream configs → camera dead).
module.add_proprietary_file('proprietary-files-camera.txt')
module.add_proprietary_file('proprietary-files-device.txt')

if __name__ == '__main__':
    utils = ExtractUtils.device_with_common(module, 'Mi8937', module.vendor)
    utils.run()
