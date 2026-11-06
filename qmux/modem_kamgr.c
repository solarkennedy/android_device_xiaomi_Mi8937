/*
 * modem_kamgr — [qmux] remove a stale keep-alive PDN config left behind by an
 * earlier carrier profile.
 *
 * The modem's keep-alive PDN manager (ds_eps_kamgr.c) keeps one 3GPP profile
 * permanently connected, re-establishing it whenever it drops. Which one comes
 * from the modem EFS file /data/ds_eps_kamgr_pdn.txt (a path inside modem EFS,
 * not Android's /data), e.g. "PROFILE_ID:3;". Only some carrier MCFG profiles
 * ship that file: of the mcfg_sw.mbn in this modem image, both Verizon profiles
 * (na/verizon/cdmaless, na/verizon/hvolte) and a few Vodafone and Deutsche
 * Telekom ones. T-Mobile, AT&T, Sprint and the rest do not, so on them
 * keep-alive is off.
 *
 * Every PVG100 started on Verizon. When a unit later moves to another carrier,
 * MCFG activates that carrier's profile, which writes its own items but does
 * not delete the Verizon one's: the file survives, still naming profile 3. On
 * Verizon, profile 3 is vzwinternet (already up, harmless). On T-Mobile it is
 * the emergency profile "sos", so the modem requested an emergency PDN every
 * ~28 s, around the clock (bench, dut1 2026-11-03/04). That kept the UE in
 * connected mode, so T-Mobile's periodic implicit detach (~every 108 min with
 * IMS on WLAN) arrived as a DETACH REQUEST "re-attach not required" cause #10,
 * which TS 24.301 handles as abnormal case b: T3402, 12 minutes without data.
 * With the file gone the UE idles, hears the same detach as a TAU REJECT #10
 * and re-attaches at once (data back in 1-4 s). PLAN-network-detach.md.
 *
 * This oneshot asks the modem which carrier profile is active (QMI PDC
 * GET_SELECTED_CONFIG) and, when that profile is not one that ships the file,
 * deletes the file: the state a clean switch to that carrier leaves.
 *
 *   - A profile that ships the file (KEEPALIVE_PROFILES) is never touched: the
 *     file is the carrier's own setting there.
 *   - FAIL-SAFE: no PDC answer, no active profile, or an unparseable ID ->
 *     do nothing ("unknown"). Only ever names the one file. Delete is
 *     verified by a fresh open (ENOENT).
 *   - The modem reads the file when its data services start, so a delete
 *     takes effect at the next boot. Status "removed" says so. EFS writes
 *     reach flash after the modem's sync (~2-3 min); a reboot before then can
 *     bring the file back, and the next run deletes it again.
 *   - Runs at boot and on SIM LOADED (a new SIM can activate another carrier
 *     profile). Mid-switch, PDC may still report the old profile; that run
 *     leaves the file and the next boot cleans it.
 *
 * Status, vendor.pepito.modem.kamgr (vendor_public_prop):
 *   removed | none | carrier | unknown | failed | off
 * vendor.pepito.modem.kamgr_config = the active PDC config ID as read,
 * vendor.pepito.modem.kamgr_file   = the file's contents before a delete.
 *
 * Transport: QMI PDC over ipc_router (libqmi_force_ipcr, as ims_enabler) and
 * DCI/EFS2 over /dev/diag (as wfc_efswrite; UNLINK is EFS2 opcode 8).
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define TAG "modem_kamgr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define STATUS_PROP     "vendor.pepito.modem.kamgr"
#define CONFIG_PROP     "vendor.pepito.modem.kamgr_config"
#define FILE_PROP       "vendor.pepito.modem.kamgr_file"
#define KAMGR_PATH      "/data/ds_eps_kamgr_pdn.txt"
#define TOTAL_BUDGET_S  180   /* modem PIL boot + PDC/EFS2 coming up */
#define RETRY_SLEEP_S   5

/*
 * Carrier profiles that ship KAMGR_PATH themselves (strings of every
 * mcfg_sw.mbn under modem_pr/mcfg/configs, C1-214411, 2026-11-04). The PDC
 * config ID is the profile's path, e.g.
 * "OTA_mcfg_sw/generic/na/tmo/commerci/mcfg_sw.mbn120:648768".
 */
static const char * const KEEPALIVE_PROFILES[] = {
	"/na/verizon/",
	"/eu/vodafone/",
	"/eu/dt/",
	NULL
};

static time_t now_mono(void)
{
	struct timespec ts;

	/* Never time(NULL): the RTC starts at 1970 and network time jumps it. */
	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;
	return ts.tv_sec;
}

static int qmux_enabled(void)
{
	char v[PROP_VALUE_MAX] = {0};

	if (__system_property_get("persist.vendor.qmux.enable", v) > 0)
		return strcmp(v, "1") == 0;
	if (__system_property_get("ro.vendor.qmux.enable", v) > 0)
		return strcmp(v, "1") == 0;
	return 0;
}

static void set_status(const char *s)
{
	__system_property_set(STATUS_PROP, s);
}

/* ------------------------------------------------------------ QMI PDC --- */

#define QMI_CLIENT_INSTANCE_ANY  0xffff
#define QMI_TIMEOUT_MS           5000
#define IDL_MINOR_MAX            512

#define PDC_REGISTER                 0x0020
#define PDC_GET_SELECTED_CONFIG      0x0022
#define PDC_CONFIG_TYPE_SW           0
#define PDC_CONFIG_TYPE_HW           1
#define PDC_IND_WAIT_MS              8000

typedef void *qmi_client_type;
typedef void *qmi_idl_service_object_type;
typedef void (*qmi_client_ind_cb)(qmi_client_type, unsigned int, void *,
                                  unsigned int, void *);

static int (*p_client_init_instance)(qmi_idl_service_object_type, uint32_t,
                                     qmi_client_ind_cb, void *, void *,
                                     uint32_t, qmi_client_type *);
static int (*p_client_send_raw_msg_sync)(qmi_client_type, unsigned int,
                                         void *, unsigned int,
                                         void *, unsigned int,
                                         unsigned int *, unsigned int);
static int (*p_client_release)(qmi_client_type);
static qmi_idl_service_object_type (*p_pdc_get_service_object)(int, int, int);

/* The last GET_SELECTED_CONFIG indication, copied by the callback thread. */
static volatile int g_ind_seen;
static uint8_t  g_ind_buf[512];
static unsigned int g_ind_len;

static void ind_cb(qmi_client_type h, unsigned int msg_id, void *buf,
                   unsigned int len, void *cbd)
{
	(void)h; (void)cbd;
	if (msg_id != PDC_GET_SELECTED_CONFIG || !buf)
		return;
	if (len > sizeof(g_ind_buf))
		len = sizeof(g_ind_buf);
	memcpy(g_ind_buf, buf, len);
	g_ind_len = len;
	__sync_synchronize();
	g_ind_seen = 1;
}

static int resolve_symbols(void)
{
	void *cci = dlopen("libqmi_cci.so", RTLD_NOW);
	void *svc = dlopen("libqmiservices.so", RTLD_NOW);

	if (!cci || !svc) {
		LOGE("dlopen failed: %s", dlerror());
		return -1;
	}
	p_client_init_instance = (int (*)(qmi_idl_service_object_type, uint32_t,
	                                  qmi_client_ind_cb, void *, void *,
	                                  uint32_t, qmi_client_type *))
	                         dlsym(cci, "qmi_client_init_instance");
	p_client_send_raw_msg_sync = (int (*)(qmi_client_type, unsigned int,
	                                      void *, unsigned int, void *,
	                                      unsigned int, unsigned int *,
	                                      unsigned int))
	                             dlsym(cci, "qmi_client_send_raw_msg_sync");
	p_client_release = (int (*)(qmi_client_type))
	                   dlsym(cci, "qmi_client_release");
	p_pdc_get_service_object = (qmi_idl_service_object_type (*)(int, int, int))
	                           dlsym(svc, "pdc_get_service_object_internal_v01");
	if (!p_client_init_instance || !p_client_send_raw_msg_sync ||
	    !p_client_release || !p_pdc_get_service_object) {
		LOGE("dlsym failed: %s", dlerror());
		return -1;
	}
	return 0;
}

static qmi_client_type pdc_connect(void)
{
	qmi_idl_service_object_type so = NULL;
	qmi_client_type c = NULL;
	char osbuf[512];

	for (int m = 0; m <= IDL_MINOR_MAX && !so; m++)
		so = p_pdc_get_service_object(1, m, 6);
	if (!so) {
		LOGE("no PDC service object");
		return NULL;
	}
	memset(osbuf, 0, sizeof(osbuf));
	if (p_client_init_instance(so, QMI_CLIENT_INSTANCE_ANY, ind_cb, NULL,
	                           osbuf, QMI_TIMEOUT_MS, &c) || !c) {
		LOGI("PDC not up yet");
		return NULL;
	}
	return c;
}

/* QMI result TLV 0x02 of a response: 0 on success. */
static int resp_ok(const uint8_t *b, unsigned int n)
{
	const uint8_t *p = b, *end = b + n;

	while (p + 3 <= end) {
		uint16_t l = (uint16_t)(p[1] | (p[2] << 8));
		if (p + 3 + l > end)
			break;
		if (p[0] == 0x02 && l >= 4)
			return (p[3] | (p[4] << 8)) == 0 ? 0 : -1;
		p += 3 + l;
	}
	return -1;
}

/*
 * The active config ID of one config type, into out (NUL-terminated).
 * Returns 1 with an ID, 0 if the modem has none selected for that type,
 * -1 on a transport failure.
 */
static int pdc_selected(qmi_client_type c, uint32_t type, uint32_t token,
                        char *out, size_t outsz)
{
	uint8_t req[16], rsp[64];
	unsigned int used = 0;
	int o = 0;

	/* 0x01 config_type (enum32), 0x10 ind_token */
	req[o++] = 0x01; req[o++] = 4; req[o++] = 0;
	memcpy(req + o, &type, 4); o += 4;
	req[o++] = 0x10; req[o++] = 4; req[o++] = 0;
	memcpy(req + o, &token, 4); o += 4;

	g_ind_seen = 0;
	if (p_client_send_raw_msg_sync(c, PDC_GET_SELECTED_CONFIG, req, o, rsp,
	                               sizeof(rsp), &used, QMI_TIMEOUT_MS) ||
	    resp_ok(rsp, used) < 0) {
		LOGE("GET_SELECTED_CONFIG(type %u) request failed", type);
		return -1;
	}
	for (int waited = 0; waited < PDC_IND_WAIT_MS; waited += 50) {
		if (g_ind_seen)
			break;
		usleep(50 * 1000);
	}
	if (!g_ind_seen) {
		LOGE("no GET_SELECTED_CONFIG indication (type %u)", type);
		return -1;
	}
	__sync_synchronize();

	/* Indication: 0x01 ind_error (u16), 0x10 token, 0x11 active config ID
	 * (u8 length + bytes), 0x12 pending config ID. */
	const uint8_t *p = g_ind_buf, *end = g_ind_buf + g_ind_len;
	int have_err = 0, err = -1;

	while (p + 3 <= end) {
		uint16_t l = (uint16_t)(p[1] | (p[2] << 8));
		const uint8_t *v = p + 3;

		if (v + l > end)
			break;
		if (p[0] == 0x01 && l >= 2) {
			have_err = 1;
			err = v[0] | (v[1] << 8);
		} else if (p[0] == 0x11 && l >= 1 && err == 0) {
			unsigned int idl = v[0];
			if (idl == 0 || idl + 1 > l || idl >= outsz)
				return 0;
			memcpy(out, v + 1, idl);
			out[idl] = '\0';
			return 1;
		}
		p = v + l;
	}
	if (have_err && err != 0)
		return 0;   /* e.g. 16 = nothing selected for this type */
	return 0;
}

/* The active carrier profile: SW first, then HW. 1 = found, 0 = none, -1 = no PDC. */
static int active_config(char *out, size_t outsz)
{
	qmi_client_type c = pdc_connect();
	uint8_t req[8], rsp[64];
	unsigned int used = 0;
	int r;

	if (!c)
		return -1;
	/* Register for config-change reporting (0x10 = 1); some stacks only
	 * send indications to registered clients. Failure here is harmless. */
	req[0] = 0x10; req[1] = 1; req[2] = 0; req[3] = 1;
	p_client_send_raw_msg_sync(c, PDC_REGISTER, req, 4, rsp, sizeof(rsp),
	                           &used, QMI_TIMEOUT_MS);

	r = pdc_selected(c, PDC_CONFIG_TYPE_SW, 1, out, outsz);
	if (r == 0)
		r = pdc_selected(c, PDC_CONFIG_TYPE_HW, 2, out, outsz);
	p_client_release(c);
	return r;
}

static int ships_keepalive(const char *id)
{
	for (int i = 0; KEEPALIVE_PROFILES[i]; i++)
		if (strstr(id, KEEPALIVE_PROFILES[i]))
			return 1;
	return 0;
}

/* --------------------------------------------------------- DCI / EFS2 --- */

#define DCI_DATA_TYPE             0x00000040
#define DIAG_IOCTL_DCI_REG        23
#define DIAG_IOCTL_DCI_DEINIT     21
#define DCI_PKT_RSP_TYPE          0
#define DIAG_CON_ALL              0x000f

struct diag_dci_reg_tbl_t {
	int32_t  client_id;
	uint16_t notification_list;
	int32_t  signal_type;
	int32_t  token;
} __attribute__((packed));

#define DIAG_SUBSYS_CMD_F   0x4B
#define DIAG_SUBSYS_FS      0x13
#define EFS2_DIAG_HELLO      0
#define EFS2_DIAG_OPEN       2
#define EFS2_DIAG_CLOSE      3
#define EFS2_DIAG_READ       4
#define EFS2_DIAG_UNLINK     8
#define EFS_O_RDONLY         00
#define EFS_ENOENT           2

#define MAXPKT  (16 * 1024)

static int      g_fd = -1;
static int32_t  g_client_id = 0;
static uint32_t g_uid = 1;

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
	reg.notification_list = DIAG_CON_ALL;
	reg.signal_type = SIGCONT;
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

static int dci_xfer(const uint8_t *req, int reqlen, uint8_t *rsp, int rsplen)
{
	static uint8_t wbuf[MAXPKT + 16];
	static uint8_t rbuf[MAXPKT * 2];
	uint32_t uid = g_uid++;
	int o = 0, n, tries = 40;

	put32(wbuf, &o, DCI_DATA_TYPE);
	put32(wbuf, &o, uid);
	put32(wbuf, &o, (uint32_t)g_client_id);
	memcpy(wbuf + o, req, reqlen);
	o += reqlen;
	if (write(g_fd, wbuf, o) < 0) {
		LOGE("write(/dev/diag): %s", strerror(errno));
		return -1;
	}
	while (tries-- > 0) {
		int off, end;

		alarm(3);
		n = read(g_fd, rbuf, sizeof(rbuf));
		alarm(0);
		if (n < 0) {
			LOGE("read(/dev/diag) uid %u: %s", uid, strerror(errno));
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
			uint32_t reclen = get32(rbuf, off + 4);
			uint32_t rid = get32(rbuf, off + 9);
			int payload = (int)reclen - 4;

			if (rectype != DCI_PKT_RSP_TYPE || payload < 0 ||
			    off + 13 + payload > end)
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

static int efs_hdr(uint8_t *b, uint16_t opcode)
{
	b[0] = DIAG_SUBSYS_CMD_F;
	b[1] = DIAG_SUBSYS_FS;
	b[2] = opcode & 0xff;
	b[3] = (opcode >> 8) & 0xff;
	return 4;
}

static int efs_check(const uint8_t *r, int n, uint16_t opcode, const char *what)
{
	if (n < 4 || r[0] != DIAG_SUBSYS_CMD_F || r[1] != DIAG_SUBSYS_FS ||
	    (uint16_t)(r[2] | (r[3] << 8)) != opcode) {
		LOGE("%s: bad response (%d bytes)", what, n);
		return -1;
	}
	return 0;
}

static int efs_hello(void)
{
	uint8_t req[64], rsp[MAXPKT];
	int o, n;

	o = efs_hdr(req, EFS2_DIAG_HELLO);
	put32(req, &o, 4);  put32(req, &o, 1024);   /* targ window   */
	put32(req, &o, 4);  put32(req, &o, 1024);   /* host window   */
	put32(req, &o, 4);  put32(req, &o, 1024);   /* iter window   */
	put32(req, &o, 1);  put32(req, &o, 1);      /* version / min */
	put32(req, &o, 1);  put32(req, &o, 0);      /* max / feature */
	n = dci_xfer(req, o, rsp, sizeof(rsp));
	return (n < 0 || efs_check(rsp, n, EFS2_DIAG_HELLO, "HELLO") < 0) ? -1 : 0;
}

/*
 * Read up to bufsz-1 bytes of path into buf (NUL-terminated).
 * Returns 1 if it exists, 0 if absent (ENOENT), -1 on any other failure.
 */
static int efs_peek(const char *path, char *buf, int bufsz)
{
	uint8_t req[MAXPKT], rsp[MAXPKT];
	int o, n;
	int32_t fd, err;

	buf[0] = '\0';
	o = efs_hdr(req, EFS2_DIAG_OPEN);
	put32(req, &o, EFS_O_RDONLY);
	put32(req, &o, 0);
	strcpy((char *)req + o, path);
	o += (int)strlen(path) + 1;
	n = dci_xfer(req, o, rsp, sizeof(rsp));
	if (n < 12 || efs_check(rsp, n, EFS2_DIAG_OPEN, "OPEN") < 0)
		return -1;
	fd = (int32_t)get32(rsp, 4);
	err = (int32_t)get32(rsp, 8);
	if (err == EFS_ENOENT)
		return 0;
	if (err != 0 || fd < 0) {
		LOGE("OPEN %s: efs_errno=%d", path, err);
		return -1;
	}

	o = efs_hdr(req, EFS2_DIAG_READ);
	put32(req, &o, (uint32_t)fd);
	put32(req, &o, (uint32_t)(bufsz - 1));
	put32(req, &o, 0);
	n = dci_xfer(req, o, rsp, sizeof(rsp));
	if (n >= 20 && efs_check(rsp, n, EFS2_DIAG_READ, "READ") == 0 &&
	    get32(rsp, 16) == 0) {
		int got = (int)get32(rsp, 12);
		if (got > n - 20)
			got = n - 20;
		if (got > bufsz - 1)
			got = bufsz - 1;
		if (got > 0)
			memcpy(buf, rsp + 20, got);
		buf[got > 0 ? got : 0] = '\0';
	}

	o = efs_hdr(req, EFS2_DIAG_CLOSE);
	put32(req, &o, (uint32_t)fd);
	dci_xfer(req, o, rsp, sizeof(rsp));
	return 1;
}

static int efs_unlink(const char *path)
{
	uint8_t req[MAXPKT], rsp[64];
	int o, n;

	o = efs_hdr(req, EFS2_DIAG_UNLINK);
	strcpy((char *)req + o, path);
	o += (int)strlen(path) + 1;
	n = dci_xfer(req, o, rsp, sizeof(rsp));
	if (n < 8 || efs_check(rsp, n, EFS2_DIAG_UNLINK, "UNLINK") < 0)
		return -1;
	if (get32(rsp, 4) != 0) {
		LOGE("UNLINK %s: efs_errno=%u", path, get32(rsp, 4));
		return -1;
	}
	return 0;
}

/* Printable copy for a property / log line: newlines and NULs dropped. */
static void printable(char *s)
{
	char *w = s;

	for (char *r = s; *r; r++)
		if (*r >= 0x20 && *r < 0x7f)
			*w++ = *r;
	*w = '\0';
}

/* --------------------------------------------------------------- main --- */

int main(void)
{
	time_t start = now_mono();
	char id[128] = {0}, content[64];
	int r;

	if (!qmux_enabled()) {
		set_status("off");
		return 0;
	}
	if (resolve_symbols() < 0) {
		set_status("unknown");
		return 0;
	}

	/* The active carrier profile; retry while the modem comes up. */
	for (;;) {
		r = active_config(id, sizeof(id));
		if (r > 0)
			break;
		if (now_mono() - start > TOTAL_BUDGET_S) {
			LOGI("no active carrier profile from PDC (%s); leaving %s alone",
			     r < 0 ? "PDC not answering" : "none selected", KAMGR_PATH);
			set_status("unknown");
			return 0;
		}
		sleep(RETRY_SLEEP_S);
	}
	printable(id);
	__system_property_set(CONFIG_PROP, id);

	if (ships_keepalive(id)) {
		LOGI("active profile %s ships the keep-alive config; left alone", id);
		set_status("carrier");
		return 0;
	}

	/* EFS2 over DCI. */
	for (;;) {
		if (dci_open() == 0) {
			if (efs_hello() == 0)
				break;
			dci_close();
		}
		if (now_mono() - start > TOTAL_BUDGET_S) {
			LOGE("modem EFS2 never answered within %ds", TOTAL_BUDGET_S);
			set_status("failed");
			return 1;
		}
		sleep(RETRY_SLEEP_S);
	}

	r = efs_peek(KAMGR_PATH, content, sizeof(content));
	if (r == 0) {
		LOGI("%s absent (profile %s)", KAMGR_PATH, id);
		set_status("none");
		dci_close();
		return 0;
	}
	if (r < 0) {
		set_status("failed");
		dci_close();
		return 1;
	}
	printable(content);
	__system_property_set(FILE_PROP, content);

	char after[64];
	if (efs_unlink(KAMGR_PATH) < 0 || efs_peek(KAMGR_PATH, after, sizeof(after)) != 0) {
		LOGE("could not delete %s", KAMGR_PATH);
		set_status("failed");
		dci_close();
		return 1;
	}
	dci_close();
	LOGI("deleted stale %s (was \"%s\"; active profile %s does not ship it); "
	     "takes effect at the next boot", KAMGR_PATH, content, id);
	set_status("removed");
	return 0;
}
