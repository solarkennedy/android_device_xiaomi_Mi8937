/*
 * modem_b12 — [qmux, Modem Tweaks] user-switched LTE Band 12 enable.
 *
 * The PVG100's RF hardware and FCC grant cover LTE B12 (700 MHz A-block), but
 * almost every unit leaves the factory with it hidden: of eight units dumped,
 * only one (6210972a) reports B12 in DMS GET_BAND_CAPABILITY. The modem offers
 * a band only when BOTH of these hold (bench, dut1, 2026-10-15/16):
 *
 *   1. legacy NV 6828 LTE_BC_CONFIG_I (u64 LTE mask) has bit 11 (= B12) set;
 *   2. the per-band RF calibration items for B12 exist in modem EFS:
 *        /nv/item_files/rfnv/00022169, 00022170, 00024237, 00027790
 *      (the four entries the B12 unit's factory `tunning` partition carries
 *      and the six-band units' do not; per-band index base+11).
 *
 * This oneshot writes both from the switch persist.gotweak.modem_b12 (Settings
 * > PVG100 Modem Tweaks). The four items ship in /vendor/etc/modem_b12/ and are
 * that one unit's factory calibration, so B12 power on any other unit is
 * borrowed, not measured: the switch is labelled experimental and off by
 * default. With them dut1 camps and registers on B12 (EARFCN 5035); user-plane
 * data on B12 is not yet verified (PLAN-android17.md, 2026-10-16).
 *
 * SAFETY
 *   - Never overwrites calibration. Items are only CREATED when absent; an
 *     existing item is left as it is. If any item exists with contents other
 *     than ours, the unit was calibrated for B12 at the factory: status
 *     "factory" (all four present, bit set) or "unsupported" (anything else),
 *     and nothing is written, ever, whatever the switch says.
 *   - The shipped items ARE unit 6210972a's factory calibration, so on that
 *     unit they read as "ours". MARKER_PATH (a modem-EFS file, survives an
 *     Android data wipe) is created before this tool first sets the bit; bit
 *     set + our items + no marker therefore also means "factory".
 *   - NV 6828 must read DONE and already carry B2/B4/B5/B13/B17 (the PVG100
 *     mask); otherwise "unsupported", no writes. Only bit 11 is changed, by
 *     read-modify-write of the item, verified by readback.
 *   - OFF clears bit 11 only. The items stay: with the bit clear the modem
 *     ignores them (dut1 ran six-band in exactly that state, 2026-10-15).
 *   - Writes reach flash only after the modem's EFS sync (~2-3 min); a restart
 *     sooner loses them. After writing, status is "saving" for FLUSH_S, then
 *     "restart". The modem reads both at its own boot, so a change takes
 *     effect on the next restart.
 *
 * Status, vendor.pepito.modem.b12 (vendor_public_prop, read by LineageParts):
 *   active | inactive | saving | restart | factory | unsupported | failed | off
 * vendor.pepito.modem.b12_boot = 1|0: whether B12 was armed when this boot's
 * first run looked, i.e. what the modem booted with (so a flip back and forth
 * without a restart still reads "restart").
 *
 * Transport: DCI over /dev/diag — EFS2 subsystem (copied from wfc_efswrite)
 * plus DIAG_NV_READ_F/NV_WRITE_F 0x26/0x27 (diag-tools/nv-probe, which proved
 * the write needs no SPC unlock on this modem).
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

#define TAG "modem_b12"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define STATUS_PROP     "vendor.pepito.modem.b12"
#define BOOT_PROP       "vendor.pepito.modem.b12_boot"
#define WANT_PROP       "persist.gotweak.modem_b12"
#define TOTAL_BUDGET_S  150   /* covers modem PIL boot + the early rmts SSR blip */
#define RETRY_SLEEP_S   5
#define FLUSH_S         180   /* modem EFS sync to flash, measured 2-3 min */

#define BLOB_DIR        "/vendor/etc/modem_b12/"
#define RFNV_DIR        "/nv/item_files/rfnv/"
#define BLOB_MAX        4096
/* In modem EFS (its /data, not Android's): "this tool armed B12 here". */
#define MARKER_PATH     "/data/pepito_modem_b12"

static const char * const g_items[] = {
	"00022169", "00022170", "00024237", "00027790", NULL
};
#define N_ITEMS 4

#define NV_LTE_BC_CONFIG  6828
#define B12_BIT           11
/* B2 B4 B5 B13 B17: every PVG100's LTE mask has these. */
#define PVG100_LTE_BASE   ((1ULL << 1) | (1ULL << 3) | (1ULL << 4) | \
                           (1ULL << 12) | (1ULL << 16))

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
		/* The modem refused the open (fd -1, errno typically ENOENT):
		 * the file is absent. (wfc_efswrite's copy of this returns -1
		 * here, because the modem reports a missing file as fd -1.) */
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

/* ------------------------------------------------------------------ NV --- */

#define DIAG_NV_READ_F   0x26
#define DIAG_NV_WRITE_F  0x27
#define NV_ITEM_SIZE     128
#define NV_DONE          0

/* Returns the NV status (0 = DONE), or -1 on a transport/diag error. */
static int nv_cmd(uint8_t cmd, uint16_t item, uint8_t *data /* 128 in/out */)
{
	uint8_t req[1 + 2 + NV_ITEM_SIZE + 2], rsp[MAXPKT];
	int n;

	req[0] = cmd;
	req[1] = item & 0xff;
	req[2] = item >> 8;
	memcpy(req + 3, data, NV_ITEM_SIZE);
	req[3 + NV_ITEM_SIZE] = 0;
	req[4 + NV_ITEM_SIZE] = 0;

	n = dci_xfer(req, sizeof(req), rsp, sizeof(rsp));
	if (n < 0)
		return -1;
	if (n >= 1 && rsp[0] != cmd) {
		LOGE("NV %u: diag error 0x%02x", item, rsp[0]);
		return -1;
	}
	if (n < (int)sizeof(req)) {
		LOGE("NV %u: short response (%d bytes)", item, n);
		return -1;
	}
	memcpy(data, rsp + 3, NV_ITEM_SIZE);
	return rsp[3 + NV_ITEM_SIZE] | (rsp[4 + NV_ITEM_SIZE] << 8);
}

static uint64_t le64(const uint8_t *d)
{
	uint64_t v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | d[i];
	return v;
}

/* Set or clear B12 in NV 6828, verified by readback. 0 / -1. */
static int nv_set_b12(const uint8_t *cur, int on)
{
	uint8_t nw[NV_ITEM_SIZE], back[NV_ITEM_SIZE];
	int st;

	memcpy(nw, cur, NV_ITEM_SIZE);
	if (on)
		nw[B12_BIT / 8] |= (uint8_t)(1u << (B12_BIT % 8));
	else
		nw[B12_BIT / 8] &= (uint8_t)~(1u << (B12_BIT % 8));

	st = nv_cmd(DIAG_NV_WRITE_F, NV_LTE_BC_CONFIG, nw);
	if (st != NV_DONE) {
		LOGE("NV 6828 write: status %d", st);
		return -1;
	}
	memset(back, 0, sizeof(back));
	st = nv_cmd(DIAG_NV_READ_F, NV_LTE_BC_CONFIG, back);
	if (st != NV_DONE || memcmp(back, nw, 8) != 0) {
		LOGE("NV 6828 verify failed (status %d)", st);
		return -1;
	}
	LOGI("NV 6828 0x%016llx -> 0x%016llx", (unsigned long long)le64(cur),
	     (unsigned long long)le64(nw));
	return 0;
}

/* ---------------------------------------------------------------- items --- */

enum item_state { ITEM_ABSENT, ITEM_OURS, ITEM_FOREIGN };

static const char *g_blob_dir = BLOB_DIR;
static uint8_t g_blob[N_ITEMS][BLOB_MAX];
static int     g_blob_len[N_ITEMS];

static int load_blobs(void)
{
	char path[128];
	int i;

	for (i = 0; i < N_ITEMS; i++) {
		FILE *f;

		snprintf(path, sizeof(path), "%s%s", g_blob_dir, g_items[i]);
		f = fopen(path, "rb");
		if (!f) {
			LOGE("open %s: %s", path, strerror(errno));
			return -1;
		}
		g_blob_len[i] = (int)fread(g_blob[i], 1, BLOB_MAX, f);
		fclose(f);
		if (g_blob_len[i] <= 0 || g_blob_len[i] >= BLOB_MAX) {
			LOGE("%s: bad size %d", path, g_blob_len[i]);
			return -1;
		}
	}
	return 0;
}

/* Classify every item. 0, or -1 on a transport error. */
static int survey_items(enum item_state *st)
{
	char path[64];
	int i;

	for (i = 0; i < N_ITEMS; i++) {
		uint8_t *buf = NULL;
		int len = 0, rc;

		snprintf(path, sizeof(path), RFNV_DIR "%s", g_items[i]);
		rc = efs_read_all(path, &buf, &len);
		if (rc < 0) {
			free(buf);
			return -1;
		}
		if (rc == 1)
			st[i] = ITEM_ABSENT;
		else if (len == g_blob_len[i] && memcmp(buf, g_blob[i], len) == 0)
			st[i] = ITEM_OURS;
		else
			st[i] = ITEM_FOREIGN;
		free(buf);
		LOGI("%s: %s", path, st[i] == ITEM_ABSENT ? "absent" :
		     st[i] == ITEM_OURS ? "present (shipped)" : "present (other)");
	}
	return 0;
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

static int want_b12(void)
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

/*
 * One pass: bring the modem to `want`. Returns 1 if it wrote anything, 0 if
 * nothing needed writing (status already published), -1 on error (status
 * published).
 */
static int apply(int want)
{
	enum item_state st[N_ITEMS];
	uint8_t nv[NV_ITEM_SIZE];
	char boot[PROP_VALUE_MAX] = {0};
	int i, ours = 0, foreign = 0, bit, armed, boot_on, nvst, marked;
	uint64_t mask;

	memset(nv, 0, sizeof(nv));
	nvst = nv_cmd(DIAG_NV_READ_F, NV_LTE_BC_CONFIG, nv);
	if (nvst < 0) {
		status("failed");
		return -1;
	}
	if (nvst != NV_DONE) {
		LOGI("NV 6828 status %d: not this modem's layout", nvst);
		status("unsupported");
		return 0;
	}
	mask = le64(nv);
	if ((mask & PVG100_LTE_BASE) != PVG100_LTE_BASE) {
		LOGI("NV 6828 0x%016llx lacks the PVG100 base bands",
		     (unsigned long long)mask);
		status("unsupported");
		return 0;
	}
	bit = (mask >> B12_BIT) & 1;

	if (survey_items(st) < 0) {
		status("failed");
		return -1;
	}
	for (i = 0; i < N_ITEMS; i++) {
		if (st[i] == ITEM_OURS)
			ours++;
		else if (st[i] == ITEM_FOREIGN)
			foreign++;
	}

	/* Factory calibration present: never touch this unit. */
	if (foreign) {
		status(foreign == N_ITEMS && bit ? "factory" : "unsupported");
		return 0;
	}

	{
		uint8_t *m = NULL;
		int mlen = 0, mrc = efs_read_all(MARKER_PATH, &m, &mlen);

		free(m);
		if (mrc < 0) {
			status("failed");
			return -1;
		}
		marked = (mrc == 0);
	}
	/* The calibration source unit itself: armed, but not by us. */
	if (bit && ours == N_ITEMS && !marked) {
		status("factory");
		return 0;
	}

	armed = bit && ours == N_ITEMS;
	if (__system_property_get(BOOT_PROP, boot) <= 0) {
		__system_property_set(BOOT_PROP, armed ? "1" : "0");
		boot_on = armed;
	} else {
		boot_on = strcmp(boot, "1") == 0;
	}

	if (want && armed) {
		status(boot_on ? "active" : "restart");
		return 0;
	}
	if (!want && !bit) {
		status(boot_on ? "restart" : "inactive");
		return 0;
	}

	if (want) {
		char path[64];

		for (i = 0; i < N_ITEMS; i++) {
			if (st[i] != ITEM_ABSENT)
				continue;
			snprintf(path, sizeof(path), RFNV_DIR "%s", g_items[i]);
			if (efs_write_all(path, g_blob[i], g_blob_len[i]) < 0) {
				status("failed");
				return -1;
			}
			LOGI("%s: created (%d bytes, verified)", path, g_blob_len[i]);
		}
		if (!marked) {
			static const uint8_t one[] = "1\n";

			if (efs_write_all(MARKER_PATH, one, 2) < 0) {
				status("failed");
				return -1;
			}
		}
		if (!bit && nv_set_b12(nv, 1) < 0) {
			status("failed");
			return -1;
		}
	} else if (nv_set_b12(nv, 0) < 0) {
		status("failed");
		return -1;
	}
	return 1;
}

int main(int argc, char **argv)
{
	time_t start = time(NULL);
	int want, rc;

	/* Bench only: a blob directory other than /vendor/etc/modem_b12/
	 * (with trailing slash), for testing a build that is not installed. */
	if (argc > 1)
		g_blob_dir = argv[1];

	if (!qmux_enabled()) {
		LOGI("qmux not enabled; nothing to do");
		status("off");
		return 0;
	}
	if (load_blobs() < 0) {
		status("failed");
		return 1;
	}

	/* The modem boots long after init: retry DCI + EFS2 hello until it
	 * answers or the budget runs out. */
	for (;;) {
		if (g_fd < 0 && dci_open() == 0) {
			if (efs_hello() == 0)
				break;
			dci_close();
		}
		if (time(NULL) - start > TOTAL_BUDGET_S) {
			LOGE("modem EFS2 never answered within %ds", TOTAL_BUDGET_S);
			status("failed");
			dci_close();
			return 1;
		}
		sleep(RETRY_SLEEP_S);
	}

	/* init `start`s this on every change of the switch, which is a no-op
	 * while a run is still flushing; so re-read the switch after each pass
	 * and go again if the user flipped it meanwhile. */
	do {
		want = want_b12();
		LOGI("switch: B12 %s", want ? "on" : "off");
		rc = apply(want);
		if (rc > 0) {
			status("saving");
			sleep(FLUSH_S);
			status("restart");
		}
	} while (rc >= 0 && want != want_b12());

	dci_close();
	return rc < 0 ? 1 : 0;
}
