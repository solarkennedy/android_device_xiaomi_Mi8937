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
 * Observable: sets vendor.qmux.ims_enabler = ok | applied | failed | off
 * and logs to logcat (tag ims_enabler).
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
#define QMI_TIMEOUT_MS  10000
#define TOTAL_BUDGET_S  150   /* covers modem PIL boot + the early rmts blip SSR */
#define RETRY_SLEEP_S   5

#define IMSS_SET_REG_MGR_CONFIG  0x0021
#define IMSS_GET_REG_MGR_CONFIG  0x0026
#define IMSS_SET_QIPCALL_CONFIG  0x0036
#define IMSS_GET_QIPCALL_CONFIG  0x0037

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

	if (!p_client_init_instance || !p_client_send_raw_msg_sync ||
	    !p_client_release || !p_imss_get_service_object) {
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
};

static const struct setting settings[] = {
	/* IMS registration administratively on. THE gate: test_mode=1 ships
	 * on stock/refurb EFS and turns IMS off (no SIP attempt at all). */
	{ "reg_mgr.ims_test_mode", IMSS_GET_REG_MGR_CONFIG, 0x13,
	                           IMSS_SET_REG_MGR_CONFIG, 0x12, 0 },
	/* QIPCALL head bools (vt / mobile_data / volte, order per IDL);
	 * bench DUT read 0,1,0 — all three must be 1 for FULL_SERVICE VoIP. */
	{ "qipcall.bool0",         IMSS_GET_QIPCALL_CONFIG, 0x11,
	                           IMSS_SET_QIPCALL_CONFIG, 0x10, 1 },
	{ "qipcall.bool1",         IMSS_GET_QIPCALL_CONFIG, 0x12,
	                           IMSS_SET_QIPCALL_CONFIG, 0x11, 1 },
	{ "qipcall.bool2",         IMSS_GET_QIPCALL_CONFIG, 0x13,
	                           IMSS_SET_QIPCALL_CONFIG, 0x12, 1 },
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

int main(void)
{
	time_t deadline = time(NULL) + TOTAL_BUDGET_S;
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
			return 0;
		}
		if (time(NULL) >= deadline) {
			LOGE("giving up after %ds", TOTAL_BUDGET_S);
			__system_property_set(STATUS_PROP, "failed");
			return 0;
		}
		sleep(RETRY_SLEEP_S);
	}
}
