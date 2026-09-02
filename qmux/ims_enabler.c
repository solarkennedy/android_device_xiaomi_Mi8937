/* ims_enabler.c — [qmux] boot oneshot that makes VoLTE/IMS self-healing on
 * pepito (PLAN-volte.md "productize" step).
 *
 * Why: the 2017 Palm modem's IMSS (svc 0x12) speaks only the v01 message
 * dialect; the A15 qcril-hal ims module is v02-only, so the framework's
 * enableIms can never reach the modem. The two settings that gate IMS
 * registration live in modem NV/EFS and were fixed live on the bench DUT
 * (2026-07-10):
 *   SET_REG_MGR_CONFIG (0x21) TLV 0x12  ims_test_mode = 0
 *   SET_QIPCALL_CONFIG (0x36) TLV 0x10/0x11/0x12 bools = 1  (vt/data/volte)
 * NV should persist, but a fresh/refurb unit, a modem EFS wipe, or another
 * user flashing this ROM starts with ims_test_mode=1 (IMS administratively
 * OFF, the "NOT_REGISTERED with reg_failure_error=0" signature). This oneshot
 * re-asserts the desired values every boot: read-compare-write, so the
 * steady-state boot is 2 read-only GETs and no NV writes.
 *
 * TLV layouts ground-truthed from the on-device IDL (parse_idl_any.py on
 * libqmiservices.so, imss object @0x25318): GET rsp TLV ids are the SET req
 * ids shifted +1 — GET_REG_MGR_CONFIG (0x26) rsp 0x13 u8 = ims_test_mode;
 * GET_QIPCALL_CONFIG (0x37) rsp 0x11/0x12/0x13 u8 = the three bools.
 *
 * Transport: plain QCCI client on the nightly /vendor/lib64/libqmi_cci.so.
 * The service stanza (init.qmux.rc) preloads libqmi_force_ipcr.so so the
 * QRTR probe fails and QCCI uses its native ipc_router backend, same as
 * every other qmux QMI client. Self-gates on the qmux enable props.
 *
 * Second phase (PLAN-vowifi.md parts 49-52, 2026-08-24): the IMS MSISDN.
 * The modem refuses VoWiFi *voice* (IMSA voip_service_status=NO_SERVICE over
 * WLAN, SMS still fine) unless IMSS GET 0x54 TLV 0x19 holds the line's
 * MSISDN. Bench units had it cached from stock-era provisioning; a virgin
 * unit has it empty and Wi-Fi calling silently comes up SMS-only. Nothing on
 * the AP side writes it on this ROM (no entitlement app; the qcril ims module
 * is v02-only). The number is learnable from the modem itself: once IMS has
 * registered on LTE, IMSA GET_REGISTRATION_STATUS (0x20) TLV 0x15 carries the
 * P-Associated-URI list incl. "tel:+1XXXXXXXXXX". We copy the digits into
 * IMSS SET 0x53 TLV 0x18 (stock's encoding: ASCII, no '+', no NUL). Verified
 * live 2026-08-24: one such write -> voice FULL_SERVICE over WLAN in 10 s and
 * a real Wi-Fi call; persists in EFS across reboots.
 *
 * Why the SIM-loaded re-run (init.qmux.rc): the IMSS store is kept per
 * subscription. A write done SIM-less at boot on a virgin unit lands only in
 * the no-SIM context and is gone once the SIM loads (parts 49/51); a write
 * with the SIM loaded persists. So init re-starts this oneshot on
 * gsm.sim.state=LOADED, and a still-running boot instance re-asserts the
 * settings once it sees IMS registered (SIM necessarily in by then).
 *
 * Observable: sets vendor.qmux.ims_enabler = ok | applied | failed | off
 * and vendor.qmux.ims_msisdn = kept | set | pending | failed, and logs to
 * logcat (tag ims_enabler).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define TAG "ims_enabler"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define STATUS_PROP     "vendor.qmux.ims_enabler"
#define MSISDN_PROP     "vendor.qmux.ims_msisdn"
#define QMI_TIMEOUT_MS  10000
#define TOTAL_BUDGET_S  150   /* covers modem PIL boot + the early rmts blip SSR */
#define MSISDN_BUDGET_S 180   /* SIM load + LTE IMS registration after boot/insert */
#define RETRY_SLEEP_S   5
#define MSISDN_MAX      32
#define MSISDN_MIN_DIGITS 7   /* shortest plausible subscriber number */

#define IMSS_SET_REG_MGR_CONFIG  0x0021
#define IMSS_GET_REG_MGR_CONFIG  0x0026
#define IMSS_SET_QIPCALL_CONFIG  0x0036
#define IMSS_GET_QIPCALL_CONFIG  0x0037
#define IMSS_GET_IMS_CONFIG      0x0054
#define IMSS_SET_IMS_CONFIG      0x0053
#define IMSS_MSISDN_GET_TLV      0x19   /* GET 0x54: ASCII digits, len 0 = unset */
#define IMSS_MSISDN_SET_TLV      0x18   /* SET 0x53: same encoding */
#define IMSA_GET_REGISTRATION_STATUS 0x0020
#define IMSA_URI_LIST_TLV        0x15   /* u8 count, then [u8 len][bytes]... */

#define QMI_CLIENT_INSTANCE_ANY  0xffff

typedef void *qmi_client_type;
typedef void *qmi_idl_service_object_type;
typedef void (*qmi_client_ind_cb)(qmi_client_type, unsigned int, void *,
                                  unsigned int, void *);

/* Classic QCCI entry points exported by the vendor libqmi_cci.so (same
 * surface the diag-tools probes use; see diag-tools/pdc-mbn-loader/
 * qcci_compat.h). Resolved via dlopen/dlsym so this builds with no import
 * library for the prebuilt vendor blobs. */
static int (*p_client_init_instance)(qmi_idl_service_object_type, uint32_t,
                                     qmi_client_ind_cb, void *, void *,
                                     uint32_t, qmi_client_type *);
static int (*p_client_send_raw_msg_sync)(qmi_client_type, unsigned int,
                                         void *, unsigned int,
                                         void *, unsigned int,
                                         unsigned int *, unsigned int);
static int (*p_client_release)(qmi_client_type);
static qmi_idl_service_object_type (*p_imss_get_service_object)(int, int, int);
static qmi_idl_service_object_type (*p_imsa_get_service_object)(int, int, int);

static void ind_cb(qmi_client_type h, unsigned int msg_id, void *buf,
                   unsigned int len, void *cbd)
{
	(void)h; (void)msg_id; (void)buf; (void)len; (void)cbd;
}

static int qmux_enabled(void)
{
	char v[PROP_VALUE_MAX] = {0};

	/* Same gate as libqmi_force_ipcr: explicit persist wins, else the
	 * libinit-set per-variant ro prop decides. */
	if (__system_property_get("persist.vendor.qmux.enable", v) > 0)
		return strcmp(v, "1") == 0;
	if (__system_property_get("ro.vendor.qmux.enable", v) > 0)
		return strcmp(v, "1") == 0;
	return 0;
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
	p_imss_get_service_object = (qmi_idl_service_object_type (*)(int, int, int))
	                            dlsym(svc, "imss_get_service_object_internal_v01");
	p_imsa_get_service_object = (qmi_idl_service_object_type (*)(int, int, int))
	                            dlsym(svc, "imsa_get_service_object_internal_v01");

	if (!p_client_init_instance || !p_client_send_raw_msg_sync ||
	    !p_client_release || !p_imss_get_service_object ||
	    !p_imsa_get_service_object) {
		LOGE("dlsym failed: %s", dlerror());
		return -1;
	}
	return 0;
}

/* Find a u8 TLV in a QMI response; returns 1 if found. If `result` is
 * non-NULL, also parses the 0x02 result TLV into it (result<<16|error). */
static int find_u8_tlv(const uint8_t *buf, unsigned int len, uint8_t want_tlv,
                       uint8_t *val, uint32_t *result)
{
	const uint8_t *p = buf, *end = buf + len;
	int found = 0;

	while (p + 3 <= end) {
		uint8_t t = p[0];
		uint16_t l = (uint16_t)(p[1] | (p[2] << 8));
		const uint8_t *v = p + 3;

		if (v + l > end)
			break;
		if (t == 0x02 && l >= 4 && result)
			*result = ((uint32_t)(v[0] | (v[1] << 8)) << 16) |
			          (uint32_t)(v[2] | (v[3] << 8));
		if (t == want_tlv && l == 1) {
			*val = v[0];
			found = 1;
		}
		p = v + l;
	}
	return found;
}

/* One IMSS setting: assert `want` via read-compare-write. Returns 0 on
 * verified-good, -1 on any failure (caller retries the whole pass). */
struct setting {
	const char *name;
	unsigned int get_msg;
	uint8_t      get_tlv;
	unsigned int set_msg;
	uint8_t      set_tlv;
	uint8_t      want;
	uint8_t      width;   /* 1 = u8 TLV; 4 = u32 TLV */
};

static const struct setting settings[] = {
	/* IMS registration administratively on. THE gate: test_mode=1 ships
	 * on stock/refurb EFS and turns IMS off (no SIP attempt at all). */
	{ "reg_mgr.ims_test_mode", IMSS_GET_REG_MGR_CONFIG, 0x13,
	                           IMSS_SET_REG_MGR_CONFIG, 0x12, 0, 1 },
	/* QIPCALL head bools (vt / mobile_data / volte, order per IDL);
	 * bench DUT read 0,1,0 — all three must be 1 for FULL_SERVICE VoIP. */
	{ "qipcall.bool0",         IMSS_GET_QIPCALL_CONFIG, 0x11,
	                           IMSS_SET_QIPCALL_CONFIG, 0x10, 1, 1 },
	{ "qipcall.bool1",         IMSS_GET_QIPCALL_CONFIG, 0x12,
	                           IMSS_SET_QIPCALL_CONFIG, 0x11, 1, 1 },
	{ "qipcall.bool2",         IMSS_GET_QIPCALL_CONFIG, 0x13,
	                           IMSS_SET_QIPCALL_CONFIG, 0x12, 1, 1 },
	/* v01 WFC IWLAN preference (Blocker-A, PLAN-vowifi.md parts 34/40/44).
	 * The framework's WFC-mode push is v02 0x6B, unimplemented on this 2017
	 * modem, so only a v01 write flips it. 1 = IWLAN-preferred (opens
	 * is_wlan_pref; the modem still keeps VoLTE on strong LTE — part 41).
	 * SET 0x53 TLV 0x15 -> GET 0x54 TLV 0x16 (verified live). u32. Arms the
	 * gate on fresh/EFS-default units so WFC works flash-and-go, not just on
	 * the hand-provisioned DUT. */
	{ "wfc.iwlan_pref",        IMSS_GET_IMS_CONFIG, 0x16,
	                           IMSS_SET_IMS_CONFIG, 0x15, 1, 4 },
	/* v01 WFC wifi_call value (PLAN-vowifi.md parts 29/45/48). MUST be
	 * stock's 1: with 2 the modem's IMS-over-IWLAN registration comes up
	 * SMS-only (IMSA voip_service_status=NO_SERVICE) — MO calls are refused
	 * by the framework and MT calls go to voicemail. Proven live on both
	 * units 2026-08-23; writing 1 brought voip_service_status to
	 * FULL_SERVICE in ~40 s and a real Wi-Fi call connected (RTP verified).
	 * The part-45 "2 works" note predates any voice testing (SMS-only bar).
	 * SET 0x53 TLV 0x14 -> GET 0x54 TLV 0x15 (verified live). u32. */
	{ "wfc.wifi_call",         IMSS_GET_IMS_CONFIG, 0x15,
	                           IMSS_SET_IMS_CONFIG, 0x14, 1, 4 },
};

static int get_u8(qmi_client_type c, unsigned int msg, uint8_t tlv,
                  uint8_t *val)
{
	uint8_t resp[2048];
	unsigned int got = 0;
	int rc = p_client_send_raw_msg_sync(c, msg, NULL, 0, resp, sizeof(resp),
	                                    &got, QMI_TIMEOUT_MS);
	if (rc)
		return -1;
	return find_u8_tlv(resp, got, tlv, val, NULL) ? 0 : -1;
}

static int set_u8(qmi_client_type c, unsigned int msg, uint8_t tlv,
                  uint8_t val)
{
	uint8_t req[4] = { tlv, 1, 0, val };
	uint8_t resp[2048];
	unsigned int got = 0;
	uint32_t result = 0xffffffff;
	uint8_t dummy;
	int rc = p_client_send_raw_msg_sync(c, msg, req, sizeof(req),
	                                    resp, sizeof(resp), &got,
	                                    QMI_TIMEOUT_MS);
	if (rc) {
		LOGE("SET 0x%04x tlv 0x%02x: send rc=%d", msg, tlv, rc);
		return -1;
	}
	find_u8_tlv(resp, got, 0x00, &dummy, &result);
	if ((result >> 16) != 0) {
		LOGE("SET 0x%04x tlv 0x%02x: result=%u error=%u",
		     msg, tlv, result >> 16, result & 0xffff);
		return -1;
	}
	return 0;
}

/* u32 TLV variants for the WFC IWLAN-preference setting (0x53/0x54 TLVs are
 * u32, unlike the u8 test_mode/qipcall bools). Little-endian. */
static int find_u32_tlv(const uint8_t *buf, unsigned int len, uint8_t want_tlv,
                        uint32_t *val, uint32_t *result)
{
	const uint8_t *p = buf, *end = buf + len;
	int found = 0;

	while (p + 3 <= end) {
		uint8_t t = p[0];
		uint16_t l = (uint16_t)(p[1] | (p[2] << 8));
		const uint8_t *v = p + 3;

		if (v + l > end)
			break;
		if (t == 0x02 && l >= 4 && result)
			*result = ((uint32_t)(v[0] | (v[1] << 8)) << 16) |
			          (uint32_t)(v[2] | (v[3] << 8));
		if (t == want_tlv && l == 4) {
			*val = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
			       ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
			found = 1;
		}
		p = v + l;
	}
	return found;
}

static int get_u32(qmi_client_type c, unsigned int msg, uint8_t tlv,
                   uint32_t *val)
{
	uint8_t resp[2048];
	unsigned int got = 0;
	if (p_client_send_raw_msg_sync(c, msg, NULL, 0, resp, sizeof(resp),
	                               &got, QMI_TIMEOUT_MS))
		return -1;
	return find_u32_tlv(resp, got, tlv, val, NULL) ? 0 : -1;
}

static int set_u32(qmi_client_type c, unsigned int msg, uint8_t tlv,
                   uint32_t val)
{
	uint8_t req[7] = { tlv, 4, 0, (uint8_t)val, (uint8_t)(val >> 8),
	                   (uint8_t)(val >> 16), (uint8_t)(val >> 24) };
	uint8_t resp[2048];
	unsigned int got = 0;
	uint32_t result = 0xffffffff;
	uint8_t dummy;
	int rc = p_client_send_raw_msg_sync(c, msg, req, sizeof(req),
	                                    resp, sizeof(resp), &got,
	                                    QMI_TIMEOUT_MS);
	if (rc) {
		LOGE("SET 0x%04x tlv 0x%02x: send rc=%d", msg, tlv, rc);
		return -1;
	}
	find_u8_tlv(resp, got, 0x00, &dummy, &result);
	if ((result >> 16) != 0) {
		LOGE("SET 0x%04x tlv 0x%02x: result=%u error=%u",
		     msg, tlv, result >> 16, result & 0xffff);
		return -1;
	}
	return 0;
}


/* Generic TLV lookup (any length). Returns pointer to the value bytes and
 * its length, or NULL. Also parses the 0x02 result TLV if `result` given. */
static const uint8_t *find_tlv(const uint8_t *buf, unsigned int len,
                               uint8_t want_tlv, uint16_t *vlen,
                               uint32_t *result)
{
	const uint8_t *p = buf, *end = buf + len, *hit = NULL;

	while (p + 3 <= end) {
		uint8_t t = p[0];
		uint16_t l = (uint16_t)(p[1] | (p[2] << 8));
		const uint8_t *v = p + 3;

		if (v + l > end)
			break;
		if (t == 0x02 && l >= 4 && result)
			*result = ((uint32_t)(v[0] | (v[1] << 8)) << 16) |
			          (uint32_t)(v[2] | (v[3] << 8));
		if (t == want_tlv) {
			hit = v;
			*vlen = l;
		}
		p = v + l;
	}
	return hit;
}

/* Connect a QCCI client to a v01 service object found via a getter that
 * needs the blob's exact IDL minor (probe 0..300, same as the diag probes). */
static qmi_client_type connect_svc(qmi_idl_service_object_type (*getter)(int, int, int),
                                   const char *name)
{
	qmi_idl_service_object_type so = NULL;
	qmi_client_type c = NULL;
	char osbuf[512];

	for (int m = 0; m <= 300 && !so; m++)
		so = getter(1, m, 6);
	if (!so) {
		LOGE("no %s service object", name);
		return NULL;
	}
	memset(osbuf, 0, sizeof(osbuf));
	if (p_client_init_instance(so, QMI_CLIENT_INSTANCE_ANY, ind_cb, NULL,
	                           osbuf, QMI_TIMEOUT_MS, &c) || !c) {
		LOGI("%s not up yet (init_instance failed)", name);
		return NULL;
	}
	return c;
}

/* Monotonic seconds for timeouts.
 *
 * ⚠️ NEVER use time(NULL) for a deadline here. This board's PMIC RTC is not
 * battery-backed: the kernel sets the clock to 1970-01-01 at every cold boot
 * ("rtc-pm8xxx: setting system clock to 1970-01-01 00:00:44 UTC"), and network
 * time sync then jumps it forward by ~56 years while we are running. Any
 * wall-clock deadline is instantly in the past, so the retry loops give up
 * immediately. Measured on 9c2e6b00 2026-09-01: the boot instance exited after
 * 12.4 s instead of honouring MSISDN_BUDGET_S=180, leaving ims_msisdn=pending
 * on a virgin unit — i.e. exactly the flash-and-go path this service exists for. */
static time_t now_mono(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;   /* cannot fail in practice; 0 just retries once more */
	return ts.tv_sec;
}

/* Is what the modem holds an actual subscriber number?
 *
 * ⚠️ A VIRGIN MODEM SHIPS TLV 0x19 = ASCII "0" (one byte), not an empty TLV.
 * Testing only for non-emptiness therefore reports "already provisioned" on
 * exactly the fresh units this code exists to fix, and VoWiFi voice stays on
 * WWAN forever (the modem gates VoWiFi voice, not SMS, on a real MSISDN).
 * Observed on 7600e051, 2026-08-31: gate 0x16 armed, TLV 0x19 = "0", voip
 * FULL_SERVICE rat=WWAN; writing the real number moved voice to WLAN in
 * seconds. So: require enough ASCII digits, and reject an all-zero string. */
static int msisdn_is_provisioned(const char *s)
{
	size_t i, n = strlen(s);
	int nonzero = 0;

	if (n < MSISDN_MIN_DIGITS || n > MSISDN_MAX)
		return 0;
	for (i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return 0;
		if (s[i] != '0')
			nonzero = 1;
	}
	return nonzero;
}

/* IMSS GET 0x54 TLV 0x19 -> ASCII MSISDN. Returns 0 with out[] filled (may
 * be empty string = unset), -1 on transport failure. */
static int get_msisdn(qmi_client_type c, char *out, size_t outsz)
{
	uint8_t resp[2048];
	unsigned int got = 0;
	uint16_t vlen = 0;
	const uint8_t *v;

	if (p_client_send_raw_msg_sync(c, IMSS_GET_IMS_CONFIG, NULL, 0, resp,
	                               sizeof(resp), &got, QMI_TIMEOUT_MS))
		return -1;
	v = find_tlv(resp, got, IMSS_MSISDN_GET_TLV, &vlen, NULL);
	out[0] = '\0';
	if (!v || vlen == 0)
		return 0;
	if (vlen >= outsz)
		vlen = (uint16_t)(outsz - 1);
	memcpy(out, v, vlen);
	out[vlen] = '\0';
	return 0;
}

/* IMSS SET 0x53 TLV 0x18 = ASCII digits, no NUL (stock encoding). */
static int set_msisdn(qmi_client_type c, const char *digits)
{
	uint8_t req[3 + MSISDN_MAX];
	uint8_t resp[2048];
	unsigned int got = 0;
	uint32_t result = 0xffffffff;
	uint16_t dummy;
	size_t n = strlen(digits);
	int rc;

	if (n == 0 || n > MSISDN_MAX)
		return -1;
	req[0] = IMSS_MSISDN_SET_TLV;
	req[1] = (uint8_t)n;
	req[2] = (uint8_t)(n >> 8);
	memcpy(req + 3, digits, n);
	rc = p_client_send_raw_msg_sync(c, IMSS_SET_IMS_CONFIG, req,
	                                (unsigned int)(3 + n), resp, sizeof(resp),
	                                &got, QMI_TIMEOUT_MS);
	if (rc) {
		LOGE("SET msisdn: send rc=%d", rc);
		return -1;
	}
	find_tlv(resp, got, 0x00, &dummy, &result);
	if ((result >> 16) != 0) {
		LOGE("SET msisdn: result=%u error=%u", result >> 16,
		     result & 0xffff);
		return -1;
	}
	return 0;
}

/* Ask IMSA for the registration URI list and pull the digits out of the
 * tel: entry. Returns 1 = digits filled, 0 = not registered / no tel URI yet,
 * -1 = IMSA unreachable. */
static int imsa_tel_digits(char *out, size_t outsz)
{
	qmi_client_type c = connect_svc(p_imsa_get_service_object, "IMSA");
	uint8_t resp[2048];
	unsigned int got = 0;
	uint16_t vlen = 0;
	const uint8_t *v, *p, *end;
	unsigned int count;
	int rc = 0;

	if (!c)
		return -1;
	if (p_client_send_raw_msg_sync(c, IMSA_GET_REGISTRATION_STATUS, NULL, 0,
	                               resp, sizeof(resp), &got, QMI_TIMEOUT_MS)) {
		p_client_release(c);
		return -1;
	}
	p_client_release(c);

	v = find_tlv(resp, got, IMSA_URI_LIST_TLV, &vlen, NULL);
	if (!v || vlen < 2)
		return 0;
	count = v[0];
	p = v + 1;
	end = v + vlen;
	for (unsigned int i = 0; i < count && p < end; i++) {
		unsigned int l = p[0];
		const uint8_t *str = p + 1;

		if (str + l > end)
			break;
		if (l > 4 && memcmp(str, "tel:", 4) == 0) {
			size_t o = 0;

			for (unsigned int k = 4; k < l && o + 1 < outsz; k++)
				if (str[k] >= '0' && str[k] <= '9')
					out[o++] = (char)str[k];
			out[o] = '\0';
			if (o >= MSISDN_MIN_DIGITS) {   /* sanity: a real subscriber number */
				rc = 1;
				break;
			}
		}
		p = str + l;
	}
	return rc;
}

/* One full pass: connect, assert every setting, verify by re-read.
 * Returns 0 = all verified at desired values; fills *wrote. */
static int pass(int *wrote)
{
	qmi_idl_service_object_type so = NULL;
	qmi_client_type c = NULL;
	char osbuf[512];
	int rc = -1;

	for (int m = 0; m <= 300 && !so; m++)
		so = p_imss_get_service_object(1, m, 6);
	if (!so) {
		LOGE("no imss service object");
		return -1;
	}

	memset(osbuf, 0, sizeof(osbuf));
	if (p_client_init_instance(so, QMI_CLIENT_INSTANCE_ANY, ind_cb, NULL,
	                           osbuf, QMI_TIMEOUT_MS, &c) || !c) {
		LOGI("IMSS not up yet (init_instance failed)");
		return -1;
	}

	for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
		const struct setting *s = &settings[i];
		uint8_t cur;

		if (s->width == 4) {   /* u32 TLV (WFC IWLAN preference) */
			uint32_t cur32;
			if (get_u32(c, s->get_msg, s->get_tlv, &cur32) == 0 &&
			    cur32 == s->want) {
				LOGI("%s already %u", s->name, s->want);
				continue;
			}
			if (set_u32(c, s->set_msg, s->set_tlv, s->want))
				goto out;
			(*wrote)++;
			if (get_u32(c, s->get_msg, s->get_tlv, &cur32) ||
			    cur32 != s->want) {
				LOGE("%s: wrote %u but readback disagrees",
				     s->name, s->want);
				goto out;
			}
			LOGI("%s: set to %u (verified)", s->name, s->want);
			continue;
		}

		if (get_u8(c, s->get_msg, s->get_tlv, &cur) == 0 &&
		    cur == s->want) {
			LOGI("%s already %u", s->name, s->want);
			continue;
		}
		if (set_u8(c, s->set_msg, s->set_tlv, s->want))
			goto out;
		(*wrote)++;
		if (get_u8(c, s->get_msg, s->get_tlv, &cur) || cur != s->want) {
			LOGE("%s: wrote %u but readback disagrees", s->name,
			     s->want);
			goto out;
		}
		LOGI("%s: set to %u (verified)", s->name, s->want);
	}
	rc = 0;
out:
	p_client_release(c);
	return rc;
}

/* Phase 2: make sure the modem holds the line's MSISDN (VoWiFi voice gate).
 * Fast path (every provisioned unit): one GET, "kept". Slow path (virgin
 * unit): wait for IMS to register on LTE, learn the number from the
 * registration URI, re-assert the settings table (the SIM is in now, so the
 * writes land in the per-subscription store), then write + verify.
 * Never fails the boot: "pending" just means no SIM/registration inside the
 * budget — the SIM-loaded trigger runs us again later. */
static void msisdn_phase(void)
{
	time_t deadline = now_mono() + MSISDN_BUDGET_S;
	char cur[MSISDN_MAX + 1], want[MSISDN_MAX + 1];
	int wrote = 0;

	__system_property_set(MSISDN_PROP, "pending");
	for (;;) {
		qmi_client_type c = connect_svc(p_imss_get_service_object, "IMSS");

		if (c && get_msisdn(c, cur, sizeof(cur)) == 0) {
			if (msisdn_is_provisioned(cur)) {
				LOGI("msisdn already set (%zu digits)", strlen(cur));
				p_client_release(c);
				__system_property_set(MSISDN_PROP, "kept");
				return;
			}
			if (cur[0])
				LOGI("msisdn placeholder \"%s\" (%zu bytes) — treating as unset",
				     cur, strlen(cur));
		}
		if (c)
			p_client_release(c);

		if (imsa_tel_digits(want, sizeof(want)) == 1) {
			/* SIM is in and IMS registered: re-assert the table so
			 * the per-subscription store gets the gate too. */
			if (pass(&wrote) == 0 && wrote)
				LOGI("re-asserted settings with SIM loaded (%d writes)", wrote);
			c = connect_svc(p_imss_get_service_object, "IMSS");
			if (c && set_msisdn(c, want) == 0 &&
			    get_msisdn(c, cur, sizeof(cur)) == 0 &&
			    strcmp(cur, want) == 0) {
				LOGI("msisdn: set from registration URI (%zu digits, verified)",
				     strlen(want));
				p_client_release(c);
				__system_property_set(MSISDN_PROP, "set");
				return;
			}
			if (c)
				p_client_release(c);
			LOGE("msisdn: write/readback failed");
			__system_property_set(MSISDN_PROP, "failed");
			return;
		}

		if (now_mono() >= deadline) {
			LOGI("msisdn: no IMS registration within %ds, leaving pending",
			     MSISDN_BUDGET_S);
			return;
		}
		sleep(RETRY_SLEEP_S);
	}
}

int main(void)
{
	time_t deadline = now_mono() + TOTAL_BUDGET_S;
	int wrote = 0;

	if (!qmux_enabled()) {
		__system_property_set(STATUS_PROP, "off");
		return 0;
	}
	if (resolve_symbols()) {
		__system_property_set(STATUS_PROP, "failed");
		return 0;
	}

	/* Retry the whole pass: covers modem PIL boot latency and the known
	 * cosmetic early-boot rmts blip (modem SSR, self-recovers). */
	for (;;) {
		wrote = 0;
		if (pass(&wrote) == 0) {
			LOGI("done: %s (%d NV writes)",
			     wrote ? "applied" : "already ok", wrote);
			__system_property_set(STATUS_PROP,
			                      wrote ? "applied" : "ok");
			break;
		}
		if (now_mono() >= deadline) {
			LOGE("giving up after %ds", TOTAL_BUDGET_S);
			__system_property_set(STATUS_PROP, "failed");
			return 0;
		}
		sleep(RETRY_SLEEP_S);
	}

	msisdn_phase();
	return 0;
}
