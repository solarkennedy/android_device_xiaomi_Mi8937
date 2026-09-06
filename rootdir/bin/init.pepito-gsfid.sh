#!/system/bin/sh
#
# init.pepito-gsfid.sh — publish the Google Services Framework device ID into
# vendor.pepito.gsf_id so the "Google Play certification" row in Pepito Tweaks
# can show it.
#
# Why this exists: the ID is what Google's uncertified-device registration page
# asks for, and a custom build needs it registered or Play Protect complains.
# GMS *serves* uncertified_status through content://com.google.android.gsf.gservices
# but WITHDRAWS android_id from third-party callers, even ones holding
# READ_GSERVICES and READ_PRIVILEGED_PHONE_STATE (verified 2026-09-04). The value
# exists only inside GMS's own 0660 app-private storage, so without this the user
# is back to enabling adb, rooting, and running a SQL query by hand.
#
# Reads the plaintext shared_prefs copy rather than gservices.db so no sqlite3
# is needed at all. Pure shell builtins - no
# grep/sed either, since vendor toybox coverage varies.
#
# Runs once at boot. If GMS has not checked in yet the file is absent and this
# is a no-op; the property fills in on the next boot, which is what the Tweaks
# row tells the user.

XML=/data/data/com.google.android.gms/shared_prefs/Checkin.xml
PROP=sys.pepito.gsf_id
TAG='<string name="android_id">'

[ -r "$XML" ] || exit 0

id=""
while IFS= read -r line; do
    case "$line" in
        *"$TAG"*)
            id=${line#*"$TAG"}
            id=${id%%<*}
            break
            ;;
    esac
done < "$XML"

# Digits only - never publish a partial parse or stray markup.
case "$id" in
    ''|*[!0-9]*) exit 0 ;;
esac

setprop "$PROP" "$id"
