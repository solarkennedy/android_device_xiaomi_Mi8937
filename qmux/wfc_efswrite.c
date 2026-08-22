/*
 * wfc_efswrite — [qmux, VoWiFi productization] toggle-gated modem-EFS write
 * that arms ps_sys "field 4" for Wi-Fi calling on a fresh unit.
 *
 * PLAN-vowifi.md parts 44-47.  The DUT-vs-Gold delta that decides whether the
 * ePDG/reverse-rmnet tunnel comes up for WFC is ONE byte in the modem's
 * packet-switched-system data config:
 *
 *     /data/ps_sys_data_configurations.txt      field  "4:1;"  (works)  vs "4:0;" (fails)
 *     /data/ps_sys_data_configurations.txt_Subscription01   ditto
 *     /data/ps_sys_data_configurations.txt_Subscription02   ditto
 *
 * These are modem-internal EFS files (module ps_sys_conf.c); no AP binary reads
 * or writes them by any name (grep of the whole vendor tree, part 47), and
 * qcril does NOT re-derive field 4 from carrier config (same ROM+SIM ->
 * different persistent value per unit, part 45 cont2).  On a stock carrier
 * device this state would be seeded by the carrier's entitlement/OMA-DM
 * provisioning flow, which our ROM structurally cannot run (Verizon OMA-DM is a
 * proprietary blob; AOSP TS.43 ships URL-less).  So, exactly like ims_enabler
 * hand-writes the WFC gate / ims_test_mode over QMI, this oneshot substitutes
 * for the missing entitlement writer by setting field 4 directly in EFS.
 *
 * Gated on the user's Wi-Fi-calling toggle: init starts it on
 * persist.sys.pepito.wfc_enabled=1 (mirrored from Settings by WfcBridge), so a
 * unit whose owner never enables WFC never gets an EFS write at all.
 *
 * SAFETY (the whole reason this is not the part-46 byte-poke):
 *   - PARSE-ANCHORED, never offset-based.  Finds the line that begins "4:" and
 *     touches only the single value byte.  Field 4 sits after field 2 (the
 *     carrier APN), so its absolute offset shifts with APN length across
 *     carriers/firmwares; a fixed offset would corrupt a neighbouring field.
 *   - SINGLE-BYTE, LENGTH-PRESERVING.  Only ever rewrites one '0' -> '1'; every
 *     other byte of the file is written back identical.  Read-back verified.
 *   - WRITE-IF-0, IDEMPOTENT.  Already-1 (the DUT, and every reboot after the
 *     first flip) -> no write.  Re-toggling WFC re-runs it -> no-op.
 *   - FAIL-SAFE.  File missing / unparseable / field 4 absent / value not a
 *     lone '0';  -> do nothing, log, exit 0.  The default action on an unknown
 *     firmware is to leave modem EFS untouched.
 *   - It ONLY ever names these three exact files.  No /nv, no calibration, no
 *     modemst; worst-case blast radius is a recoverable /data data-config.
 *
 * Transport is DCI over /dev/diag with the EFS2 subsystem, the exact opcodes
 * and framing proven in diag-tools/efs2-probe (this file is a self-contained,
 * minimal, write-if-0 subset of that tool).  No QMI/ipc_router here, so it does
 * NOT link libqmi_force_ipcr.
 *
 * ---------------------------------------------------------------------------
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define TAG "wfc_efswrite"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define STATUS_PROP     "vendor.qmux.wfc_efswrite"
#define TOTAL_BUDGET_S  150   /* covers modem PIL boot + the early rmts SSR blip */
#define RETRY_SLEEP_S   5

/* The three EFS files carrying ps_sys field 4 (main + per-subscription). */
static const char * const g_targets[] = {
	"/data/ps_sys_data_configurations.txt",
	"/data/ps_sys_data_configurations.txt_Subscription01",
	"/data/ps_sys_data_configurations.txt_Subscription02",
	NULL
};

/* ---- kernel ABI (include/linux/diagchar.h) ---- */
#define DCI_DATA_TYPE             0x00000040
#define DIAG_IOCTL_DCI_REG        23
#define DIAG_IOCTL_DCI_DEINIT     21
#define DCI_PKT_RSP_TYPE          0

#define DIAG_CON_APSS   0x0001
#define DIAG_CON_MPSS   0x0002
#define DIAG_CON_LPASS  0x0004
#define DIAG_CON_WCNSS  0x0008
#define DIAG_CON_ALL    (DIAG_CON_APSS | DIAG_CON_MPSS | DIAG_CON_LPASS | \
                         DIAG_CON_WCNSS)

struct diag_dci_reg_tbl_t {
	int32_t  client_id;
	uint16_t notification_list;
	int32_t  signal_type;
	int32_t  token;
} __attribute__((packed));

/* ---- DIAG / EFS2 ABI ---- */
#define DIAG_SUBSYS_CMD_F   0x4B
#define DIAG_SUBSYS_FS      0x13

#define EFS2_DIAG_HELLO      0
#define EFS2_DIAG_OPEN       2
#define EFS2_DIAG_CLOSE      3
#define EFS2_DIAG_READ       4
#define EFS2_DIAG_WRITE      5

/* EFS2 oflag bits — octal, from AMSS fs_fcntl.h, NOT the host <fcntl.h>. */
#define EFS_O_RDONLY         00
#define EFS_O_WRONLY         01
#define EFS_O_CREAT        0100
#define EFS_O_TRUNC       01000

#define MAXPKT  (16 * 1024)

static int      g_fd = -1;
static int32_t  g_client_id = 0;
static uint32_t g_uid = 1;
static int      g_read_chunk = 512;
static int      g_write_chunk = 512;

static void put32(uint8_t *b, int *o, uint32_t v)
{
	b[*o + 0] = v & 0xff;
	b[*o + 1] = (v >> 8) & 0xff;
	b[*o + 2] = (v >> 16) & 0xff;
	b[*o + 3] = (v >> 24) & 0xff;
	*o += 4;
}

static uint32_t get32(const uint8_t *b, int o)
{
	return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) |
	       ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

static void alrm_handler(int sig) { (void)sig; }

/* ------------------------------------------------------------------ DCI --- */

/* Returns 0 on success, -1 on failure (no exit — main() retries). */
static int dci_open(void)
{
	struct diag_dci_reg_tbl_t reg;
	struct sigaction sa;
	int rc;

	g_fd = open("/dev/diag", O_RDWR | O_LARGEFILE);
	if (g_fd < 0) {
		LOGE("open(/dev/diag): %s", strerror(errno));
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = alrm_handler;   /* no SA_RESTART: alarm must EINTR read() */
	sigaction(SIGALRM, &sa, NULL);

	memset(&reg, 0, sizeof(reg));
	reg.client_id = 0;
	reg.notification_list = DIAG_CON_ALL;
	reg.signal_type = SIGCONT;
	reg.token = 0;

	rc = ioctl(g_fd, DIAG_IOCTL_DCI_REG, (unsigned long)&reg);
	if (rc <= 0) {
		LOGE("DIAG_IOCTL_DCI_REG failed: rc=%d errno=%s", rc, strerror(errno));
		close(g_fd);
		g_fd = -1;
		return -1;
	}
	g_client_id = rc;
	return 0;
}

static void dci_close(void)
{
	int32_t cid = g_client_id;
	if (g_fd >= 0) {
		if (g_client_id > 0)
			ioctl(g_fd, DIAG_IOCTL_DCI_DEINIT, (unsigned long)&cid);
		close(g_fd);
		g_fd = -1;
	}
}

/* Send one DCI request, collect the matching response. Returns length or -1. */
static int dci_xfer(const uint8_t *req, int reqlen, uint8_t *rsp, int rsplen)
{
	static uint8_t wbuf[MAXPKT + 16];
	static uint8_t rbuf[MAXPKT * 2];
	uint32_t uid = g_uid++;
	int o = 0, n, deadline_tries = 40;

	put32(wbuf, &o, DCI_DATA_TYPE);
	put32(wbuf, &o, uid);
	put32(wbuf, &o, (uint32_t)g_client_id);
	memcpy(wbuf + o, req, reqlen);
	o += reqlen;

	n = write(g_fd, wbuf, o);
	if (n < 0) {
		LOGE("write(/dev/diag): %s", strerror(errno));
		return -1;
	}

	while (deadline_tries-- > 0) {
		int off, end;

		alarm(3);
		n = read(g_fd, rbuf, sizeof(rbuf));
		alarm(0);
		if (n < 0) {
			if (errno == EINTR)
				LOGE("read timeout waiting for uid %u", uid);
			else
				LOGE("read(/dev/diag): %s", strerror(errno));
			return -1;
		}
		if (n < 12 || get32(rbuf, 0) != DCI_DATA_TYPE)
			continue;

		end = 12 + (int)get32(rbuf, 8);
		if (end > n)
			end = n;
		off = 12;
		while (off + 13 <= end) {
			int32_t  rectype = (int32_t)get32(rbuf, off);
			uint32_t reclen, rid;
			int payload;

			if (rectype != DCI_PKT_RSP_TYPE)
				break;
			reclen = get32(rbuf, off + 4);
			rid    = get32(rbuf, off + 9);
			payload = (int)reclen - 4;
			if (payload < 0 || off + 13 + payload > end)
				break;
			if (rid == uid) {
				if (payload > rsplen)
					payload = rsplen;
				memcpy(rsp, rbuf + off + 13, payload);
				return payload;
			}
			off += 13 + payload;
		}
	}
	LOGE("gave up waiting for uid %u", uid);
	return -1;
}

/* ----------------------------------------------------------------- EFS2 --- */

static int efs_hdr(uint8_t *b, uint16_t opcode)
{
	int o = 0;
	b[o++] = DIAG_SUBSYS_CMD_F;
	b[o++] = DIAG_SUBSYS_FS;
	b[o++] = opcode & 0xff;
	b[o++] = (opcode >> 8) & 0xff;
	return o;
}

static int efs_check(const uint8_t *r, int n, uint16_t opcode, const char *what)
{
	if (n < 4) {
		LOGE("%s: short response (%d bytes)", what, n);
		return -1;
	}
	if (r[0] != DIAG_SUBSYS_CMD_F || r[1] != DIAG_SUBSYS_FS ||
	    (uint16_t)(r[2] | (r[3] << 8)) != opcode) {
		LOGE("%s: unexpected response header %02x %02x %02x %02x",
		     what, r[0], r[1], r[2], r[3]);
		return -1;
	}
	return 0;
}

/* Handshake + window negotiation. Returns 0 if the EFS2 subsystem answers. */
static int efs_hello(void)
{
	uint8_t req[64], rsp[MAXPKT];
	int o, n;

	o = efs_hdr(req, EFS2_DIAG_HELLO);
	put32(req, &o, 4);      put32(req, &o, 1024);   /* targ window   */
	put32(req, &o, 4);      put32(req, &o, 1024);   /* host window   */
	put32(req, &o, 4);      put32(req, &o, 1024);   /* iter window   */
	put32(req, &o, 1);      put32(req, &o, 1);      /* version / min */
	put32(req, &o, 1);      put32(req, &o, 0);      /* max / feature */

	n = dci_xfer(req, o, rsp, sizeof(rsp));
	if (n < 0 || efs_check(rsp, n, EFS2_DIAG_HELLO, "HELLO") < 0)
		return -1;

	if (n >= 44) {
		uint32_t twb = get32(rsp, 8), hwb = get32(rsp, 16);
		if (twb >= 64 && twb < MAXPKT) {
			g_read_chunk = (int)twb - 64;
			if (g_read_chunk > 1024)
				g_read_chunk = 1024;
		}
		if (hwb >= 128 && hwb < MAXPKT) {
			g_write_chunk = (int)hwb - 64;
			if (g_write_chunk > 512)
				g_write_chunk = 512;
		}
	}
	return 0;
}

/* Returns the modem descriptor slot, or -1, with *err set to its errno. */
static int32_t efs_open_flags(const char *path, int32_t oflag, int32_t mode,
			      int32_t *err)
{
	uint8_t req[MAXPKT], rsp[MAXPKT];
	int o, n;

	o = efs_hdr(req, EFS2_DIAG_OPEN);
	put32(req, &o, (uint32_t)oflag);
	put32(req, &o, (uint32_t)mode);
	strncpy((char *)req + o, path, sizeof(req) - o - 1);
	o += (int)strlen(path) + 1;

	n = dci_xfer(req, o, rsp, sizeof(rsp));
	if (n < 0 || efs_check(rsp, n, EFS2_DIAG_OPEN, "OPEN") < 0 || n < 12)
		return -1;
	*err = (int32_t)get32(rsp, 8);
	return (int32_t)get32(rsp, 4);
}

static void efs_close(int32_t fd)
{
	uint8_t req[32], rsp[MAXPKT];
	int o = efs_hdr(req, EFS2_DIAG_CLOSE);
	put32(req, &o, (uint32_t)fd);
	dci_xfer(req, o, rsp, sizeof(rsp));
}

/*
 * Read the whole file into *buf (caller frees). Returns 0 on success, -1 on a
 * transport/modem error, or +1 if the file simply does not exist (efs_errno).
 */
static int efs_read_all(const char *path, uint8_t **buf, int *outlen)
{
	uint8_t req[64], rsp[MAXPKT];
	uint8_t *acc = NULL;
	int acclen = 0, acccap = 0;
	int32_t fd, err = 0;
	uint32_t offset = 0;

	*buf = NULL;
	*outlen = 0;

	fd = efs_open_flags(path, EFS_O_RDONLY, 0, &err);
	if (fd < 0)
		return -1;
	if (err != 0) {
		/* modem errno set (typically ENOENT) — file absent, not fatal */
		return 1;
	}

	for (;;) {
		int o = efs_hdr(req, EFS2_DIAG_READ);
		int n, got;
		int32_t rerr;

		put32(req, &o, (uint32_t)fd);
		put32(req, &o, (uint32_t)g_read_chunk);
		put32(req, &o, offset);

		n = dci_xfer(req, o, rsp, sizeof(rsp));
		if (n < 0 || efs_check(rsp, n, EFS2_DIAG_READ, "READ") < 0 || n < 20)
			break;
		got  = (int32_t)get32(rsp, 12);
		rerr = (int32_t)get32(rsp, 16);
		if (rerr != 0) {
			LOGE("READ %s @%u: efs_errno=%d", path, offset, rerr);
			break;
		}
		if (got <= 0)
			break;
		if (20 + got > n)
			got = n - 20;
		if (acclen + got > acccap) {
			acccap = (acclen + got) * 2 + 4096;
			acc = realloc(acc, acccap);
			if (!acc) {
				LOGE("oom");
				efs_close(fd);
				return -1;
			}
		}
		memcpy(acc + acclen, rsp + 20, got);
		acclen += got;
		offset += (uint32_t)got;
		if (got < g_read_chunk)
			break;   /* short read == EOF */
	}

	efs_close(fd);
	*buf = acc;
	*outlen = acclen;
	return 0;
}

/* One EFS2_DIAG_WRITE. Returns bytes written (== len) or -1. */
static int efs_write_chunk(int32_t fd, uint32_t offset, const uint8_t *data,
			   int len)
{
	uint8_t req[MAXPKT], rsp[MAXPKT];
	int o, n;
	int32_t wrote, werr;

	if (len <= 0 || len > MAXPKT - 32)
		return -1;

	o = efs_hdr(req, EFS2_DIAG_WRITE);
	put32(req, &o, (uint32_t)fd);
	put32(req, &o, offset);
	memcpy(req + o, data, len);
	o += len;

	n = dci_xfer(req, o, rsp, sizeof(rsp));
	if (n < 0 || efs_check(rsp, n, EFS2_DIAG_WRITE, "WRITE") < 0 || n < 20)
		return -1;
	wrote = (int32_t)get32(rsp, 12);
	werr  = (int32_t)get32(rsp, 16);
	if (werr != 0 || wrote != len) {
		LOGE("WRITE @%u (%d): bytes_written=%d efs_errno=%d",
		     offset, len, wrote, werr);
		return -1;
	}
	return wrote;
}

/* Overwrite path with buf[len] (O_WRONLY|O_CREAT|O_TRUNC) then verify. 0/-1. */
static int efs_write_all(const char *path, const uint8_t *buf, int len)
{
	uint8_t *back = NULL;
	int backlen = 0, rc = -1;
	uint32_t offset = 0;
	int32_t fd, err = 0;

	fd = efs_open_flags(path, EFS_O_WRONLY | EFS_O_CREAT | EFS_O_TRUNC,
			    0644, &err);
	if (fd < 0 || err != 0) {
		LOGE("OPEN-for-write %s failed: fd=%d efs_errno=%d", path, fd, err);
		return -1;
	}

	while ((int)offset < len) {
		int chunk = len - (int)offset;
		int wrote;

		if (chunk > g_write_chunk)
			chunk = g_write_chunk;
		wrote = efs_write_chunk(fd, offset, buf + offset, chunk);
		if (wrote <= 0) {
			LOGE("write aborted at offset %u", offset);
			efs_close(fd);
			return -1;
		}
		offset += (uint32_t)wrote;
	}
	efs_close(fd);

	/* read back and compare byte-for-byte */
	if (efs_read_all(path, &back, &backlen) != 0 || !back) {
		LOGE("VERIFY: could not read %s back", path);
		return -1;
	}
	if (backlen == len && memcmp(back, buf, len) == 0)
		rc = 0;
	else
		LOGE("VERIFY FAILED for %s (wrote %d, read back %d)",
		     path, len, backlen);
	free(back);
	return rc;
}

/* ------------------------------------------------------------ field 4 --- */

/*
 * Process one target file. Returns:
 *    1  wrote field 4 = 1 (was 0)
 *    0  no change needed / fail-safe skip (file absent, already 1, or the
 *       field-4 shape is anything other than a lone '0';)
 *   -1  transport error or a write that failed to verify
 */
static int process_file(const char *path)
{
	uint8_t *buf = NULL;
	int len = 0, rc;
	int i, valpos = -1;

	rc = efs_read_all(path, &buf, &len);
	if (rc == 1) {
		LOGI("%s: absent, skip", path);
		return 0;
	}
	if (rc < 0 || !buf || len <= 0) {
		LOGE("%s: read failed", path);
		free(buf);
		return (rc < 0) ? -1 : 0;
	}

	/* Parse-anchor: find a line that begins "4:". */
	for (i = 0; i + 2 < len; i++) {
		int at_line_start = (i == 0) || (buf[i - 1] == '\n');
		if (at_line_start && buf[i] == '4' && buf[i + 1] == ':') {
			valpos = i + 2;
			break;
		}
	}
	if (valpos < 0 || valpos + 1 >= len) {
		LOGI("%s: no field 4, skip (fail-safe)", path);
		free(buf);
		return 0;
	}

	/* The value must be a single digit terminated by ';'. */
	if (buf[valpos + 1] != ';') {
		LOGI("%s: field 4 not a single-char value ('%c%c'...), skip "
		     "(fail-safe)", path, buf[valpos], buf[valpos + 1]);
		free(buf);
		return 0;
	}
	if (buf[valpos] == '1') {
		LOGI("%s: field 4 already 1", path);
		free(buf);
		return 0;
	}
	if (buf[valpos] != '0') {
		LOGI("%s: field 4 = '%c' (not 0/1), skip (fail-safe)",
		     path, buf[valpos]);
		free(buf);
		return 0;
	}

	/* The only mutation: one byte, length preserved, everything else kept. */
	buf[valpos] = '1';
	if (efs_write_all(path, buf, len) < 0) {
		free(buf);
		return -1;
	}
	LOGI("%s: field 4 0 -> 1 (verified)", path);
	free(buf);
	return 1;
}

/* --------------------------------------------------------------- gate --- */

static int qmux_enabled(void)
{
	char v[PROP_VALUE_MAX] = {0};

	if (__system_property_get("persist.vendor.qmux.enable", v) > 0)
		return strcmp(v, "1") == 0;
	if (__system_property_get("ro.vendor.qmux.enable", v) > 0)
		return strcmp(v, "1") == 0;
	return 0;
}

int main(void)
{
	time_t start = time(NULL);
	int connected = 0;

	if (!qmux_enabled()) {
		LOGI("qmux not enabled; nothing to do");
		__system_property_set(STATUS_PROP, "off");
		return 0;
	}

	/* Retry the DCI open + EFS2 hello until the modem answers (it boots long
	 * after init) or the budget runs out. */
	for (;;) {
		if (g_fd < 0 && dci_open() == 0) {
			if (efs_hello() == 0) {
				connected = 1;
				break;
			}
			dci_close();   /* diag up but EFS2 not answering yet */
		}
		if (time(NULL) - start > TOTAL_BUDGET_S) {
			LOGE("modem EFS2 never answered within %ds", TOTAL_BUDGET_S);
			__system_property_set(STATUS_PROP, "error");
			dci_close();
			return 1;
		}
		sleep(RETRY_SLEEP_S);
	}
	(void)connected;

	int applied = 0, errors = 0, i;
	for (i = 0; g_targets[i]; i++) {
		int r = process_file(g_targets[i]);
		if (r > 0)
			applied += r;
		else if (r < 0)
			errors++;
	}

	dci_close();

	if (errors)
		__system_property_set(STATUS_PROP, "error");
	else if (applied)
		__system_property_set(STATUS_PROP, "applied");
	else
		__system_property_set(STATUS_PROP, "ok");

	LOGI("done: %d file(s) armed, %d error(s)", applied, errors);
	return errors ? 1 : 0;
}
