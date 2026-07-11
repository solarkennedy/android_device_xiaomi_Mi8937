#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

from extract_utils.file import File
from extract_utils.fixups_blob import (
    BlobFixupCtx,
    blob_fixup,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

namespace_imports = [
    'device/xiaomi/mithorium-common',
]

module = ExtractUtilsModule(
    'Mi8937',
    'xiaomi',
    namespace_imports=namespace_imports,
    add_firmware_proprietary_file=True,
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
