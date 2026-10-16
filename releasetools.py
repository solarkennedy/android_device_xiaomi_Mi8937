# SPDX-License-Identifier: Apache-2.0
#
# OTA extensions for Mi8937 (pepito): every OTA also writes the boot logo.
#
# splash.img is the first 128 KiB of the splash partition (the SPLASH!!
# header and its frames); the rest of the partition is left as it is. The
# build puts it in target-files RADIO/ (Android.mk, add-radio-file).

import os

import common

SPLASH = "splash.img"
SPLASH_DEV = "/dev/block/by-name/splash"


def FullOTA_InstallEnd(info):
  _WriteSplash(info, info.input_zip, info.input_tmp)


def IncrementalOTA_InstallEnd(info):
  _WriteSplash(info, info.target_zip, info.target_tmp)


def _WriteSplash(info, target_zip, target_tmp):
  path = os.path.join(target_tmp or "", "RADIO", SPLASH)
  if target_tmp and os.path.exists(path):
    with open(path, "rb") as f:
      data = f.read()
  elif target_zip is not None and "RADIO/" + SPLASH in target_zip.namelist():
    data = target_zip.read("RADIO/" + SPLASH)
  else:
    return  # a product without a splash image (Android.mk)
  if data[:8] != b"SPLASH!!":
    raise ValueError("RADIO/%s is not a splash image" % SPLASH)
  common.ZipWriteStr(info.output_zip, "firmware-update/" + SPLASH, data)
  info.script.Print("Writing the boot logo...")
  info.script.AppendExtra('package_extract_file("firmware-update/%s", "%s");'
                          % (SPLASH, SPLASH_DEV))
