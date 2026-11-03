/*
 * modem_evs — [qmux, Modem Tweaks] keep VoLTE EVS calls at wideband.
 *
 * On this modem + ADSP an EVS super-wideband (SWB, 32 kHz) voice session that
 * starts while a voice calibration is registered crashes the ADSP
 * ("Excep :0:Exception detected"), and ~2 s later the modem asserts in
 * qpaudio_svs.c:4475: the call drops and IMS goes down. A calibration is
 * registered at session start whenever the call starts on a device with a
 * valid ACDB pair, the speaker is selected before EVS comes up, or the kernel
 * still caches the speaker calibration from the previous call. EVS-WB,
 * AMR-WB and AMR never crash; neither does switching device on an SWB session
 * that is already running. Reproduced on every modem build we have (C1-162128
 * x2, C1-214411), with dual- and single-mic, on T-Mobile and on Verizon with
 * SWB forced (bench, dut1 + new2, 2026-10-31; PLAN-android17.md, "Bug
 * 2026-10-30"; diag-tools/ims-codec/captures/).
 *
 * The carrier MCFG profile sets the EVS limits in the modem EFS item
 * /nv/item_files/ims/qipcall_evs_codec_config (32 bytes, layout otherwise
 * unknown). Byte 9 is the maximum EVS bandwidth: 02 -> the network negotiates
 * EVS-WB, 03 -> EVS-SWB (isolated one byte at a time on dut1). T-Mobile's
 * profile ships 03; the Verizon profiles 02 or no EVS at all.
 *
 * This oneshot caps byte 9 at 02 unless the user allows super-wideband
 * (persist.gotweak.modem_evs_swb=1, Settings > PVG100 Modem Tweaks; default 0):
 *
 *   - Only the T-Mobile layout is touched: every byte but 9 must match
 *     TMO_LAYOUT, the only layout byte 9 was isolated on. Verizon's hvolte
 *     profile is laid out differently (01 00 10 00 00 00 6d 00 02 04 ...) and
 *     has byte 9 = 04; 20261101 capped it, and Verizon calls to EVS-capable
 *     phones then failed with "sdp parse failed" (ImsReasonInfo 1502), even
 *     after a rollback, until modemst was restored. Any other layout is
 *     "unsupported" and left alone.
 *   - An item of another layout that this tool did cap (the marker matches) is
 *     put back: byte 9 gets the saved value again, whatever the switch says.
 *   - A profile without EVS, or already at WB, is left alone (status "none").
 *   - The original byte is saved first in MARKER_PATH (modem EFS, survives an
 *     Android data wipe); allowing SWB restores exactly that byte. A carrier
 *     profile reload (SIM swap) rewrites the item; the next run caps it again.
 *   - The T-Mobile layout at WB is ours even without a marker: no carrier
 *     profile in the modem ships it (all 78 MCFG profiles of C1-162128 and
 *     C1-222870 swept, 2026-11-03: the 8 layouts in KNOWN_LAYOUTS). A lost
 *     marker is rewritten so the switch can still restore SWB.
 *   - An item shorter than 32 bytes whose bytes are a prefix of what the
 *     marker says we wrote is our own interrupted write (O_TRUNC, then the
 *     data never landed): the item is rebuilt from the marker, as the profile
 *     asked for it, and the next pass caps it again if it should be.
 *   - Read-modify-write of byte 9 only, written only when it differs,
 *     verified by readback.
 *   - Writes reach flash only after the modem's EFS sync (~2-3 min) and the
 *     IMS stack reads the item at its own start, so a change takes effect on
 *     the next restart: status "saving" for FLUSH_S, then "restart". After
 *     the flush the item is read again: a profile reload (MCFG activation on
 *     a new SIM, whose init trigger is a no-op `start` while this is still
 *     running) or a modem restart during the flush would otherwise leave a
 *     T-Mobile profile at SWB until the next boot.
 *
 * Status, vendor.pepito.modem.evs (vendor_public_prop, read by LineageParts):
 *   capped | swb | none | saving | restart | unsupported | failed | off
 * vendor.pepito.modem.evs_boot = capped|swb|none: what the modem booted with.
 * For field reports ("did modem_evs ever write here?"), also per boot:
 *   vendor.pepito.modem.evs_item   = the item as last read, 32 bytes hex
 *   vendor.pepito.modem.evs_marker = absent | cleared | stale | <orig byte>
 *                                    (the marker matches: capped from it)
 *   vendor.pepito.modem.evs_write  = none | <what the last write this boot
 *                                    did, e.g. "cap 3->2", "undo 2->4">
 * and the same in logcat (tag modem_evs). A "cleared" marker means a cap
 * was undone or SWB allowed on this unit at some point.
 *
 * Transport: DCI over /dev/diag, EFS2 subsystem, copied from modem_b12.
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

#define TAG "modem_evs"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define STATUS_PROP     "vendor.pepito.modem.evs"
#define BOOT_PROP       "vendor.pepito.modem.evs_boot"
#define ITEM_PROP       "vendor.pepito.modem.evs_item"
#define MARKER_PROP     "vendor.pepito.modem.evs_marker"
#define WRITE_PROP      "vendor.pepito.modem.evs_write"
#define WANT_PROP       "persist.gotweak.modem_evs_swb"
#define TOTAL_BUDGET_S  150   /* covers modem PIL boot + the early rmts SSR blip */
#define RETRY_SLEEP_S   5
#define FLUSH_S         180   /* modem EFS sync to flash, measured 2-3 min */
#define MAX_WRITES      3     /* per run: cap, re-cap after a lost write, done */

#define EVS_CFG_PATH    "/nv/item_files/ims/qipcall_evs_codec_config"
#define EVS_CFG_LEN     32
#define EVS_BW_BYTE     9
#define EVS_BW_WB       0x02
#define EVS_BW_MAX      0x04  /* anything above is not a layout we know */
/* The T-Mobile profile's item (tmo/commerci in C1-162128, C1-214411 and
 * C1-222870), byte 9 aside; the only layout whose byte 9 is known. */
static const uint8_t TMO_LAYOUT[EVS_CFG_LEN] = {
	0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
};
#define TMO_BW          0x03  /* what tmo/commerci ships in byte 9 */

static int tmo_layout(const uint8_t *cfg)
{
	int i;

	for (i = 0; i < EVS_CFG_LEN; i++)
		if (i != EVS_BW_BYTE && cfg[i] != TMO_LAYOUT[i])
			return 0;
	return 1;
}

/*
 * Every distinct qipcall_evs_codec_config a carrier profile in this modem
 * ships (all 78 mcfg_sw.mbn of C1-162128 and C1-222870, identical in both;
 * diag-tools/ims-codec/mbn-codec.py, 2026-11-03). Bytes 10..31 are zero
 * except where shown. Used to name the item in the log and to tell a
 * natively-WB profile from one we capped: the T-Mobile layout with byte 9 =
 * 02 is in no profile, so an item like that without a marker is still ours.
 * The live item can also differ from every profile (new2's Verizon item is
 * 00 00 10 00 00 00 6d 00 03 02), so a miss here only means "not from the
 * MCFG as shipped".
 */
static const struct {
	const char *name;
	uint8_t bytes[10];
	int byte16;
} KNOWN_LAYOUTS[] = {
	{ "na/tmo/commerci",      { 0x00, 0x00, 0x10, 0, 0, 0, 0x00, 0, 0x02, 0x03 }, 0 },
	{ "na/verizon/hvolte",    { 0x01, 0x00, 0x10, 0, 0, 0, 0x6d, 0, 0x02, 0x04 }, 0x6d },
	{ "na/verizon/cdmaless",  { 0x00, 0x00, 0x10, 0, 0, 0, 0x00, 0, 0x00, 0x02 }, 0 },
	{ "apac/dcm/commerci",    { 0x00, 0x00, 0x10, 0, 0, 0, 0x00, 0, 0xff, 0x04 }, 0 },
	{ "apac/kddi/commerci",   { 0x00, 0x00, 0x18, 0, 0, 0, 0x00, 0, 0x02, 0x03 }, 0 },
	{ "apac/sbm/commerci",    { 0x00, 0x00, 0x78, 0, 0, 0, 0x7e, 0, 0x02, 0x03 }, 0 },
	{ "china/cmcc+ct lab",    { 0x00, 0x00, 0x78, 0, 0, 0, 0x00, 0, 0x02, 0x03 }, 0 },
	{ "korea/kt+lgu+skt",     { 0x00, 0x00, 0x40, 0, 0, 0, 0x6e, 0, 0x00, 0x06 }, 0 },
};

/* The profile cfg is, as shipped, or NULL. */
static const char *known_layout(const uint8_t *cfg)
{
	size_t k;
	int i;

	for (k = 0; k < sizeof(KNOWN_LAYOUTS) / sizeof(KNOWN_LAYOUTS[0]); k++) {
		if (memcmp(cfg, KNOWN_LAYOUTS[k].bytes, 10) != 0)
			continue;
		for (i = 10; i < EVS_CFG_LEN; i++)
			if (cfg[i] != (i == 16 ? KNOWN_LAYOUTS[k].byte16 : 0))
				break;
		if (i == EVS_CFG_LEN)
			return KNOWN_LAYOUTS[k].name;
	}
	return NULL;
}

static const char *hex(const uint8_t *b, int len)
{
	static char s[2 * EVS_CFG_LEN + 1];
	int i;

	if (len > EVS_CFG_LEN)
		len = EVS_CFG_LEN;
	for (i = 0; i < len; i++)
		snprintf(s + 2 * i, 3, "%02x", b[i]);
	s[2 * len] = 0;
	return s;
}

/* What a pass that wrote something settles to (main publishes it once the
 * write has been flushed). */
static const char *g_done = "none";

/* In modem EFS (its /data, not Android's): the byte 9 we capped from. */
#define MARKER_PATH     "/data/pepito_evs_bw"

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
#define EFS_ENOENT            2   /* EFS2 errno: POSIX numbering */

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

/* Returns the modem descriptor slot (-1 when the modem refused, *err = its
 * errno), or EFS_XFER_FAIL when the request itself got no answer. */
#define EFS_XFER_FAIL  INT32_MIN
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
		return EFS_XFER_FAIL;
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
	if (fd == EFS_XFER_FAIL)
		return -1;
	if (fd < 0 || err != 0) {
		/* The modem refused the open (fd -1, errno ENOENT): the file is
		 * absent. (wfc_efswrite's copy of this returns -1 here, because
		 * the modem reports a missing file as fd -1.) Any other errno is
		 * not "no such profile" and must not read as "none": say so. */
		if (err != 0 && err != EFS_ENOENT) {
			LOGE("OPEN %s refused: fd=%d efs_errno=%d", path, fd, err);
			return -1;
		}
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

static int want_swb(void)
{
	char v[PROP_VALUE_MAX] = {0};

	__system_property_get(WANT_PROP, v);
	return strcmp(v, "1") == 0;
}

static void status(const char *s)
{
	__system_property_set(STATUS_PROP, s);
	LOGI("status: %s", s);
}

/* What the modem runs with for a given byte 9 and marker. */
static const char *state_of(int bw, int saved)
{
	if (bw > EVS_BW_WB)
		return "swb";
	return saved > EVS_BW_WB ? "capped" : "none";
}

/*
 * After a write has been saved: a restart is only needed if the modem now
 * differs from what it booted with (a flip and back within one boot lands
 * where it started).
 */
static const char *settled_status(const char *now)
{
	char boot[PROP_VALUE_MAX] = {0};

	__system_property_get(BOOT_PROP, boot);
	return strcmp(boot, now) ? "restart" : now;
}

/*
 * The marker is 1 + EVS_CFG_LEN bytes: the byte 9 this tool capped from, then
 * the whole item as it wrote it (byte 9 = WB). It only counts for an item that
 * still matches it: either exactly (still capped) or with byte 9 back at the
 * saved value (the same profile reloaded). Anything else - another carrier's
 * profile after a SIM swap, a profile that is natively WB - is not ours, so
 * "allow SWB" can never raise a profile above what that profile itself asked
 * for. A marker whose first byte is 0 was cleared after a restore or an undo.
 */
#define MARKER_ABSENT   0
#define MARKER_CLEARED  1
#define MARKER_SET      2
struct marker {
	int     state;
	uint8_t orig;                   /* the byte 9 we capped from */
	uint8_t item[EVS_CFG_LEN];      /* the item as we wrote it */
};

/* Returns 0, or -1 on a transport error. */
static int load_marker(struct marker *mk)
{
	uint8_t *m = NULL;
	int mlen = 0, rc;

	memset(mk, 0, sizeof(*mk));
	rc = efs_read_all(MARKER_PATH, &m, &mlen);
	if (rc < 0)
		return -1;
	if (rc == 0 && m && mlen == 1 + EVS_CFG_LEN) {
		mk->orig = m[0];
		memcpy(mk->item, m + 1, EVS_CFG_LEN);
		mk->state = (m[0] > EVS_BW_WB && m[0] <= EVS_BW_MAX) ?
			    MARKER_SET : MARKER_CLEARED;
	} else if (rc == 0) {
		LOGE("%s: %d bytes, not a marker; ignored", MARKER_PATH, mlen);
	}
	free(m);
	return 0;
}

/* The saved byte when the marker is ours for this item, else 0. */
static int marker_saved(const struct marker *mk, const uint8_t *cfg)
{
	uint8_t want[EVS_CFG_LEN];

	if (mk->state != MARKER_SET)
		return 0;
	if (memcmp(mk->item, cfg, EVS_CFG_LEN) == 0)
		return mk->orig;                /* still capped */
	memcpy(want, mk->item, EVS_CFG_LEN);
	want[EVS_BW_BYTE] = mk->orig;
	if (memcmp(want, cfg, EVS_CFG_LEN) == 0)
		return mk->orig;                /* same profile, reloaded */
	return 0;
}

static int write_marker(uint8_t orig, const uint8_t *capped)
{
	uint8_t m[1 + EVS_CFG_LEN];

	m[0] = orig;
	memcpy(m + 1, capped, EVS_CFG_LEN);
	return efs_write_all(MARKER_PATH, m, sizeof(m));
}

/* MARKER_PROP + log: what the marker says about the item just read. */
static void publish_marker(const struct marker *mk, int saved)
{
	char v[8];
	const char *s = mk->state == MARKER_SET ? "stale" :
			mk->state == MARKER_CLEARED ? "cleared" : "absent";

	if (saved) {
		snprintf(v, sizeof(v), "%d", saved);
		s = v;
	}
	__system_property_set(MARKER_PROP, s);
	if (mk->state == MARKER_SET)
		LOGI("marker: capped from %d, item %s: %s", mk->orig,
		     hex(mk->item, EVS_CFG_LEN), saved ? "matches" : "stale");
	else
		LOGI("marker: %s", s);
}

/* WRITE_PROP + log: a verified write of the item. */
static void note_write(const char *what, int from, int to)
{
	char v[PROP_VALUE_MAX];

	if (from < 0)
		snprintf(v, sizeof(v), "%s ->%d", what, to);
	else
		snprintf(v, sizeof(v), "%s %d->%d", what, from, to);
	__system_property_set(WRITE_PROP, v);
	LOGI("%s: byte %d %s (verified)", EVS_CFG_PATH, EVS_BW_BYTE, v);
}

/*
 * One pass: bring byte 9 to what the switch asks for. Returns 1 if it wrote
 * anything, 0 if nothing needed writing (status already published), -1 on
 * error (status published).
 */
static int apply(int swb)
{
	uint8_t *cfg = NULL;
	char boot[PROP_VALUE_MAX] = {0};
	struct marker mk;
	const char *what, *name;
	int len = 0, rc, bw, saved, target;
	uint8_t mark;

	rc = efs_read_all(EVS_CFG_PATH, &cfg, &len);
	if (rc < 0) {
		status("failed");
		return -1;
	}
	__system_property_set(ITEM_PROP, rc > 0 ? "absent" : hex(cfg, len));
	if (rc > 0) {
		/* No EVS settings in this profile: nothing to cap. */
		LOGI("%s: absent", EVS_CFG_PATH);
		status("none");
		__system_property_set(BOOT_PROP, "none");
		return 0;
	}
	name = len == EVS_CFG_LEN ? known_layout(cfg) : NULL;
	LOGI("%s: %d bytes %s (%s)", EVS_CFG_PATH, len, hex(cfg, len),
	     name ? name : "not a profile as shipped");

	if (load_marker(&mk) < 0) {
		free(cfg);
		status("failed");
		return -1;
	}

	if (len < EVS_CFG_LEN && mk.state == MARKER_SET &&
	    memcmp(cfg, mk.item, len) == 0) {
		/* Our own write, interrupted after its O_TRUNC: rebuild the
		 * item as the profile asked for it. The next pass caps it
		 * again if it should be. */
		uint8_t item[EVS_CFG_LEN];

		LOGE("%s: %d bytes = a prefix of what we wrote; rebuilding it "
		     "from the marker", EVS_CFG_PATH, len);
		publish_marker(&mk, mk.orig);
		memcpy(item, mk.item, EVS_CFG_LEN);
		item[EVS_BW_BYTE] = mk.orig;
		free(cfg);
		if (efs_write_all(EVS_CFG_PATH, item, EVS_CFG_LEN) < 0) {
			status("failed");
			return -1;
		}
		note_write("rebuild", -1, mk.orig);
		g_done = "restart";
		return 1;
	}

	if (len != EVS_CFG_LEN || cfg[EVS_BW_BYTE] > EVS_BW_MAX) {
		LOGI("%s: %d bytes, byte %d = %d: not a layout we know",
		     EVS_CFG_PATH, len, EVS_BW_BYTE, len > EVS_BW_BYTE ? cfg[EVS_BW_BYTE] : -1);
		publish_marker(&mk, 0);
		free(cfg);
		status("unsupported");
		return 0;
	}
	bw = cfg[EVS_BW_BYTE];
	saved = marker_saved(&mk, cfg);

	if (!saved && bw == EVS_BW_WB && tmo_layout(cfg)) {
		/* The T-Mobile layout at WB ships in no profile (KNOWN_LAYOUTS):
		 * capped by an earlier run whose marker is gone, or stale from
		 * another SIM. Put the marker back so "allow SWB" can restore
		 * it; if this write is lost to an early reboot the next run
		 * does it again. */
		LOGI("%s: T-Mobile layout at WB with no matching marker: capped "
		     "by an earlier run; rewriting the marker", EVS_CFG_PATH);
		if (write_marker(TMO_BW, cfg) < 0) {
			free(cfg);
			status("failed");
			return -1;
		}
		mk.state = MARKER_SET;
		mk.orig = TMO_BW;
		memcpy(mk.item, cfg, EVS_CFG_LEN);
		saved = TMO_BW;
	}
	publish_marker(&mk, saved);

	if (!tmo_layout(cfg)) {
		/* Already at WB or below: nothing to cap, whatever the layout. */
		const char *alone = bw > EVS_BW_WB ? "unsupported" : "none";

		if (__system_property_get(BOOT_PROP, boot) <= 0)
			__system_property_set(BOOT_PROP, saved ? state_of(bw, saved) : alone);
		if (!saved) {
			LOGI("%s: not the T-Mobile layout; left alone", EVS_CFG_PATH);
			free(cfg);
			/* "restart" after an undo this boot, until the reboot. */
			__system_property_get(BOOT_PROP, boot);
			status(strcmp(boot, alone) ? "restart" : alone);
			return 0;
		}
		/* Ours, on a layout we must not touch: undo the cap. */
		target = saved;
		what = "undo";
		g_done = "unsupported";
		LOGI("%s: not the T-Mobile layout; undoing the earlier cap", EVS_CFG_PATH);
		goto write;
	}

	if (__system_property_get(BOOT_PROP, boot) <= 0)
		__system_property_set(BOOT_PROP, state_of(bw, saved));

	if (!swb && bw > EVS_BW_WB) {
		/* Cap: remember the profile as it asked (byte 9 and the item as
		 * we will leave it), then write WB. */
		mark = (uint8_t)bw;
		cfg[EVS_BW_BYTE] = EVS_BW_WB;
		if (saved != bw && write_marker(mark, cfg) < 0) {
			free(cfg);
			status("failed");
			return -1;
		}
		cfg[EVS_BW_BYTE] = mark;
		target = EVS_BW_WB;
		what = "cap";
		g_done = "capped";
	} else if (swb && bw == EVS_BW_WB && saved) {
		/* Allow SWB again: restore exactly the byte we capped from. */
		target = saved;
		what = "restore";
		g_done = "swb";
	} else {
		__system_property_get(BOOT_PROP, boot);
		const char *now = state_of(bw, saved);
		status(strcmp(boot, now) ? "restart" : now);
		free(cfg);
		return 0;
	}

write:
	cfg[EVS_BW_BYTE] = (uint8_t)target;
	if (efs_write_all(EVS_CFG_PATH, cfg, len) < 0) {
		free(cfg);
		status("failed");
		return -1;
	}
	note_write(what, bw, target);
	free(cfg);

	if (target != EVS_BW_WB) {
		/* Restored: an all-zero marker no longer matches any item. */
		uint8_t none[EVS_CFG_LEN];

		memset(none, 0, sizeof(none));
		if (write_marker(0, none) < 0)
			/* Not fatal: the item is back as the profile asked, and a
			 * marker left behind only ever matches this very item. */
			LOGE("%s: could not clear the marker; the item is restored anyway",
			     MARKER_PATH);
	}
	return 1;
}

/* DCI + EFS2 hello, retried until the modem answers or the budget runs out.
 * The modem boots long after init. 0 / -1. */
static int efs_connect(void)
{
	time_t start = time(NULL);

	for (;;) {
		if (g_fd < 0 && dci_open() == 0) {
			if (efs_hello() == 0)
				return 0;
			dci_close();
		}
		if (time(NULL) - start > TOTAL_BUDGET_S) {
			LOGE("modem EFS2 never answered within %ds", TOTAL_BUDGET_S);
			dci_close();
			return -1;
		}
		sleep(RETRY_SLEEP_S);
	}
}

int main(void)
{
	char v[PROP_VALUE_MAX] = {0};
	const char *settled = NULL;
	int swb, rc, writes = 0;

	if (!qmux_enabled()) {
		LOGI("qmux not enabled; nothing to do");
		status("off");
		return 0;
	}
	if (__system_property_get(WRITE_PROP, v) <= 0)
		__system_property_set(WRITE_PROP, "none");

	if (efs_connect() < 0) {
		status("failed");
		return 1;
	}

	/* init `start`s this on every change of the switch and on SIM load,
	 * both no-ops while a run is still flushing; so after a write, and
	 * whenever the switch moved meanwhile, read the item again and go
	 * again. A modem restart during the flush drops the DCI client, so
	 * reconnect first. */
	do {
		swb = want_swb();
		LOGI("switch: EVS super-wideband %s", swb ? "allowed" : "capped to WB");
		rc = apply(swb);
		if (rc < 0 && settled) {
			/* The write before it was verified and flushed: keep
			 * saying so rather than "failed" for the re-check. */
			LOGE("re-check failed; keeping status %s", settled);
			status(settled);
			break;
		}
		if (rc > 0) {
			status("saving");
			sleep(FLUSH_S);
			settled = settled_status(g_done);
			status(settled);
			if (++writes >= MAX_WRITES) {
				LOGE("%d writes this run; not checking again", writes);
				break;
			}
			dci_close();
			if (efs_connect() < 0) {
				LOGE("no modem after the flush; keeping status %s", settled);
				break;
			}
		}
	} while (rc > 0 || (rc == 0 && swb != want_swb()));

	dci_close();
	return rc < 0 ? 1 : 0;
}
