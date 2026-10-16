LOCAL_PATH := $(call my-dir)

ifeq ($(TARGET_DEVICE),Mi8937)

# The boot logo aboot draws: the first 128 KiB of the splash partition (the
# rest is left on the phone as it is). Lands in target-files RADIO/, and
# releasetools.py writes it in every OTA. Made with tools/splash/ in the
# lineageos-pepito landing repo.
$(call add-radio-file,splash/splash.img)

endif
