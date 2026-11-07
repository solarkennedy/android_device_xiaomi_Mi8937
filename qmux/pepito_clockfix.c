// pepito_clockfix.c — [pepito] keep the AP wall clock across boots without
// writing the PMIC RTC.
//
// The PM8937 RTC is a read-only raw counter on this platform (device tree
// has no allow-set-time; stock locked it the same way because the modem
// shares the RTC and keeps its own offsets to it in modem EFS). The kernel
// therefore boots the system clock at "1970 + counter" and Android waits for
// NTP or NITZ (~38 s with Wi-Fi, indefinitely without). Qualcomm's answer is
// time_daemon + the TimeService app persisting wall-minus-counter offsets; we
// do not ship that stack and the daemon alone is harmful (it resets the clock
// to 1970 when no offset was ever stored).
//
// This is the minimal AP-only version, two oneshots driven by init
// (init.pepito-clockfix.rc):
//   save    — once the clock is plausible (boot_completed, shutdown): store
//             offset = wall - counter together with the counter.
//   restore — at post-fs-data, if the clock is still at 1970: set it to
//             counter + offset. Skipped when the counter went backwards
//             (battery died, RTC restarted) — then we are simply no worse
//             than before, and the network corrects the clock as today.
// The offset is a constant while the RTC runs, so nothing needs to watch the
// clock continuously; later NTP/NITZ corrections are accepted as usual and
// the next save refreshes the offset. Outcome in vendor.qmux.clockfix.
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>

#define TAG "pepito_clockfix"
#define STATE_DIR "/data/vendor/time"
#define STATE_FILE STATE_DIR "/pepito_clockfix"
#define STATE_TMP STATE_FILE ".tmp"
#define STATUS_PROP "vendor.qmux.clockfix"
/* A clock before this is "unset" (1970 + RTC counter). 2024-01-01 UTC. */
#define PLAUSIBLE_EPOCH 1704067200L

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static void status(const char *s)
{
	__system_property_set(STATUS_PROP, s);
}

/* RTC counter in seconds (the driver presents the raw count as a date). */
static int read_rtc(long *counter)
{
	struct rtc_time rt;
	struct tm tm;
	time_t t;
	int fd, rc;

	fd = open("/dev/rtc0", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		LOGE("open /dev/rtc0: %s", strerror(errno));
		return -1;
	}
	rc = ioctl(fd, RTC_RD_TIME, &rt);
	close(fd);
	if (rc < 0) {
		LOGE("RTC_RD_TIME: %s", strerror(errno));
		return -1;
	}
	memset(&tm, 0, sizeof(tm));
	tm.tm_sec = rt.tm_sec;
	tm.tm_min = rt.tm_min;
	tm.tm_hour = rt.tm_hour;
	tm.tm_mday = rt.tm_mday;
	tm.tm_mon = rt.tm_mon;
	tm.tm_year = rt.tm_year;
	t = timegm(&tm);
	if (t < 0) {
		LOGE("RTC time does not convert");
		return -1;
	}
	*counter = (long)t;
	return 0;
}

static int read_state(long *offset, long *counter_saved, long *wall_saved)
{
	FILE *f = fopen(STATE_FILE, "re");
	int n;

	if (!f)
		return -1;
	n = fscanf(f, "v1 %ld %ld %ld", offset, counter_saved, wall_saved);
	fclose(f);
	return n == 3 ? 0 : -1;
}

static int do_save(void)
{
	long counter, now = (long)time(NULL);
	FILE *f;

	if (now < PLAUSIBLE_EPOCH) {
		LOGI("save: clock not set yet (%ld), keeping previous offset",
		     now);
		status("save-skipped-clock-unset");
		return 0;
	}
	if (read_rtc(&counter) < 0) {
		status("save-failed-rtc");
		return 1;
	}
	f = fopen(STATE_TMP, "we");
	if (!f) {
		LOGE("open %s: %s", STATE_TMP, strerror(errno));
		status("save-failed-write");
		return 1;
	}
	fprintf(f, "v1 %ld %ld %ld\n", now - counter, counter, now);
	if (fclose(f) != 0 || rename(STATE_TMP, STATE_FILE) != 0) {
		LOGE("write %s: %s", STATE_FILE, strerror(errno));
		unlink(STATE_TMP);
		status("save-failed-write");
		return 1;
	}
	LOGI("save: offset %ld s (counter %ld, wall %ld)", now - counter,
	     counter, now);
	status("saved");
	return 0;
}

static int do_restore(void)
{
	long offset, counter_saved, wall_saved, counter, target;
	long now = (long)time(NULL);
	struct timeval tv;
	char buf[32];
	time_t t;

	if (now >= PLAUSIBLE_EPOCH) {
		LOGI("restore: clock already set (%ld)", now);
		status("restore-not-needed");
		return 0;
	}
	if (read_state(&offset, &counter_saved, &wall_saved) < 0) {
		LOGI("restore: no saved offset yet");
		status("restore-no-state");
		return 0;
	}
	if (read_rtc(&counter) < 0) {
		status("restore-failed-rtc");
		return 1;
	}
	if (counter < counter_saved) {
		LOGI("restore: RTC counter %ld < saved %ld, RTC was reset; leaving clock alone",
		     counter, counter_saved);
		status("restore-skipped-rtc-reset");
		return 0;
	}
	target = counter + offset;
	if (target < PLAUSIBLE_EPOCH || target < wall_saved) {
		LOGE("restore: implausible target %ld (saved wall %ld)", target,
		     wall_saved);
		status("restore-skipped-bad-state");
		return 0;
	}
	tv.tv_sec = target;
	tv.tv_usec = 0;
	if (settimeofday(&tv, NULL) < 0) {
		LOGE("settimeofday: %s", strerror(errno));
		status("restore-failed-settime");
		return 1;
	}
	t = target;
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", gmtime(&t));
	LOGI("restore: clock set to %s UTC from RTC counter %ld + offset %ld (%ld s since last save)",
	     buf, counter, offset, counter - counter_saved);
	status("restored");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "save"))
		return do_save();
	if (argc == 2 && !strcmp(argv[1], "restore"))
		return do_restore();
	fprintf(stderr, "usage: %s save|restore\n", argv[0]);
	return 2;
}
