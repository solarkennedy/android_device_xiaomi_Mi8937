/* wfc_wlan_bridge.c — [qmux, VoWiFi productization] persistent daemon that
 * feeds the legacy modem the WLAN availability + measurement reports it needs
 * to bring up IWLAN / the ePDG tunnel for Wi-Fi calling on pepito.
 *
 * Background (PLAN-vowifi.md parts 34-41): this 2017 Palm modem does IWLAN the
 * legacy (modem-centric) way — the ePDG / IPsec tunnel lives inside the modem,
 * and the AP must tell it "Wi-Fi is available, here is its SSID/BSSID/quality"
 * over the DSD QMI service (svc, tool 6). On a stock Qualcomm device the
 * Connectivity Engine (cnd) does this; LineageOS ships no cnd, and the stock
 * A8 cnd drags a heavy A8 CNE/HIDL/QMI closure (ABI-risky on 4.19/A16). This is
 * a small purpose-built replacement: it polls wlan0 and, on each fresh
 * association, PRIMES the modem for ~75 s by sending every FAST_SEC:
 *   DSD 0x34 TLV 0x13 = 1        wifi radio switch on
 *   DSD 0x20 (WLAN_AVAILABLE)    stock-shaped, live BSSID/IPv4/SSID/freq
 *   DSD 0x3c (WLAN measurement)  WQE profile 1, incrementing seq, BSSID/freq/RSSI
 * Without this the modem never learns Wi-Fi exists, so IWLAN never registers
 * and no Wi-Fi call can be set up (proven live: WFC-on + Wi-Fi + gate alone ->
 * stays LTE, r_rmnet DOWN). Feeding is only needed to ESTABLISH: once IWLAN is
 * up the modem self-maintains the ePDG tunnel with no further reports (proven
 * live 2026-08-21), so after priming the daemon backs off to a cheap wlan0 watch
 * and only re-primes on a disassociation/roam. Whether it runs at all is gated by
 * init on the user's Wi-Fi-calling toggle (see init.qmux.rc + WfcBridge, which
 * mirrors the Settings toggle to persist.sys.pepito.wfc_enabled). The IWLAN->VoLTE
 * handover on good LTE is separate and *correct* (part 41) and is NOT handled here.
 *
 * Transport: QCCI on the nightly libqmi_cci, dlopen()ed (no import lib for the
 * vendor prebuilt); the ipc_router backend is forced by linking
 * libqmi_force_ipcr, whose socket() shim interposes the dlopen()ed libs too
 * (never LD_PRELOAD — bionic scrubs it under Enforcing; see qmux.te). Self-gates
 * on the qmux enable prop; status in vendor.qmux.wfc_bridge =
 * off | searching | feeding | error.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/wireless.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define TAG "wfc_wlan_bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define STATUS_PROP     "vendor.qmux.wfc_bridge"
#define IFACE           "wlan0"
/* Feeding is only needed to ESTABLISH IWLAN; once up, the modem self-maintains
 * the ePDG tunnel without further reports (proven live 2026-08-21: feeder killed
 * + Wi-Fi up -> IMS SMS still delivered over IWLAN). So we prime on association,
 * then stop and just watch wlan0 cheaply for a disassociation/roam that needs a
 * fresh prime. This drops the daemon from "3 QMI sends every 3 s forever" to a
 * bounded burst per association plus a light idle poll. */
#define FAST_SEC        3       /* poll/feed cadence while searching or priming */
#define SLOW_SEC        15      /* wlan0 watch cadence once IWLAN is up (no QMI) */
#define PRIME_CYCLES    25      /* feed for ~75 s per association (covers the ~60 s */
                                /* worst-case establish seen under airplane), then stop */
#define REPRIME_SEC     300     /* insurance: brief re-prime every 5 min while up, in */
                                /* case the modem ever drops IWLAN with Wi-Fi still up */
#define QMI_TIMEOUT_MS  8000
#define QMI_CLIENT_INSTANCE_ANY 0xffff

#define DSD_SET_WIFI_RADIO_REQ 0x0034  /* TLV 0x13 = wifi radio switch */
#define DSD_WLAN_AVAILABLE_REQ 0x0020
#define DSD_WLAN_MEAS_REQ      0x003c
#define WQE_PROFILE_ID         1       /* the armed WQE profile on pepito; 3/6 -> err 22 */
#define REPORT_RSSI            (-50)   /* strong. Handover is NOT RSSI-driven (part 41); */
                                       /* a plausible good value keeps the modem from roving. */

typedef void *qmi_client_type;
typedef void *qmi_idl_service_object_type;
typedef void (*qmi_client_ind_cb)(qmi_client_type, unsigned int, void *,
                                  unsigned int, void *);

static int (*p_init_instance)(qmi_idl_service_object_type, uint32_t,
                              qmi_client_ind_cb, void *, void *,
                              uint32_t, qmi_client_type *);
static int (*p_send_raw)(qmi_client_type, unsigned int, void *, unsigned int,
                         void *, unsigned int, unsigned int *, unsigned int);
static int (*p_release)(qmi_client_type);
static qmi_idl_service_object_type (*p_dsd_get_so)(int, int, int);

static void ind_cb(qmi_client_type h, unsigned int m, void *b,
                   unsigned int l, void *c)
{ (void)h; (void)m; (void)b; (void)l; (void)c; }

static int qmux_enabled(void)
{
	char v[PROP_VALUE_MAX] = {0};
	if (__system_property_get("persist.vendor.qmux.enable", v) > 0)
		return strcmp(v, "1") == 0;
	if (__system_property_get("ro.vendor.qmux.enable", v) > 0)
		return strcmp(v, "1") == 0;
	return 0;
}

/* Set STATUS_PROP only on change (avoids property-set spam each poll). */
static void set_status(const char *s, const char **cur)
{
	if (*cur && strcmp(*cur, s) == 0) return;
	__system_property_set(STATUS_PROP, s);
	*cur = s;
}

static int resolve_syms(void)
{
	void *cci = dlopen("libqmi_cci.so", RTLD_NOW);
	void *svc = dlopen("libqmiservices.so", RTLD_NOW);
	if (!cci || !svc) { LOGE("dlopen failed: %s", dlerror()); return -1; }
	p_init_instance = (int (*)(qmi_idl_service_object_type, uint32_t,
	                           qmi_client_ind_cb, void *, void *, uint32_t,
	                           qmi_client_type *))
	                  dlsym(cci, "qmi_client_init_instance");
	p_send_raw = (int (*)(qmi_client_type, unsigned int, void *, unsigned int,
	                      void *, unsigned int, unsigned int *, unsigned int))
	             dlsym(cci, "qmi_client_send_raw_msg_sync");
	p_release = (int (*)(qmi_client_type)) dlsym(cci, "qmi_client_release");
	p_dsd_get_so = (qmi_idl_service_object_type (*)(int, int, int))
	               dlsym(svc, "dsd_get_service_object_internal_v01");
	if (!p_init_instance || !p_send_raw || !p_release || !p_dsd_get_so) {
		LOGE("dlsym failed: %s", dlerror());
		return -1;
	}
	return 0;
}

/* Append a QMI TLV; returns new offset. */
static int put_tlv(uint8_t *r, int off, uint8_t id, const void *v, uint16_t l)
{
	r[off] = id;
	r[off + 1] = (uint8_t)(l & 0xff);
	r[off + 2] = (uint8_t)(l >> 8);
	memcpy(r + off + 3, v, l);
	return off + 3 + l;
}

/* ---- live wlan0 state via wireless-extensions (WE 22 confirmed on wlan0) ---- */
struct wlan_info {
	uint8_t  bssid[6];
	char     ssid[33];
	int      ssid_len;
	uint16_t freq_mhz;
	uint8_t  ip_le[4];   /* IPv4 as the modem wants it: little-endian of the u32 */
	int      connected;
};

static uint16_t iwfreq_to_mhz(const struct iw_freq *f)
{
	double hz = (double)f->m;
	for (int i = 0; i < f->e; i++) hz *= 10.0;
	if (hz < 100000.0) {                 /* driver reported a channel, not a freq */
		int ch = (int)hz;
		if (ch >= 1 && ch <= 13) return (uint16_t)(2407 + ch * 5);
		if (ch == 14) return 2484;
		if (ch >= 36) return (uint16_t)(5000 + ch * 5);
		return 0;
	}
	return (uint16_t)(hz / 1e6);
}

static int read_wlan(struct wlan_info *w)
{
	memset(w, 0, sizeof(*w));
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return -1;

	struct iwreq wrq;
	memset(&wrq, 0, sizeof(wrq));
	strncpy(wrq.ifr_ifrn.ifrn_name, IFACE, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIWAP, &wrq) < 0) { close(s); return -1; }
	memcpy(w->bssid, wrq.u.ap_addr.sa_data, 6);
	int zero = 1;
	for (int i = 0; i < 6; i++) if (w->bssid[i]) zero = 0;
	if (zero) { close(s); return 0; }    /* not associated */
	w->connected = 1;

	memset(&wrq, 0, sizeof(wrq));
	strncpy(wrq.ifr_ifrn.ifrn_name, IFACE, IFNAMSIZ - 1);
	wrq.u.essid.pointer = w->ssid;
	wrq.u.essid.length = 32;
	if (ioctl(s, SIOCGIWESSID, &wrq) == 0) {
		w->ssid_len = wrq.u.essid.length;
		if (w->ssid_len > 32) w->ssid_len = 32;
		w->ssid[w->ssid_len] = 0;
	}

	memset(&wrq, 0, sizeof(wrq));
	strncpy(wrq.ifr_ifrn.ifrn_name, IFACE, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIWFREQ, &wrq) == 0)
		w->freq_mhz = iwfreq_to_mhz(&wrq.u.freq);

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_ifrn.ifrn_name, IFACE, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
		/* sa_data[2..5] = IPv4 in network (big-endian) order; the modem TLV
		 * wants the little-endian u32, i.e. the network bytes reversed. */
		const uint8_t *net = (const uint8_t *)ifr.ifr_addr.sa_data + 2;
		w->ip_le[0] = net[3];
		w->ip_le[1] = net[2];
		w->ip_le[2] = net[1];
		w->ip_le[3] = net[0];
	}
	close(s);
	return 0;
}

/* ---- DSD sends ---- */
static qmi_client_type g_c;

/* Returns QMI result (0 = success), or a negative transport error. */
static int dsd_send(unsigned int msg, uint8_t *req, unsigned int len)
{
	uint8_t resp[2048];
	unsigned int got = 0;
	int rc = p_send_raw(g_c, msg, req, len, resp, sizeof(resp), &got,
	                    QMI_TIMEOUT_MS);
	if (rc) return -1;
	const uint8_t *p = resp, *e = resp + got;
	while (p + 3 <= e) {
		uint8_t t = p[0];
		uint16_t l = (uint16_t)(p[1] | (p[2] << 8));
		const uint8_t *v = p + 3;
		if (v + l > e) break;
		if (t == 0x02 && l >= 4) return (v[0] | (v[1] << 8)); /* result */
		p = v + l;
	}
	return 0;
}

static void send_wifi_switch(void)
{
	uint8_t r[8];
	uint8_t one[4] = { 1, 0, 0, 0 };
	int o = put_tlv(r, 0, 0x13, one, 4);
	dsd_send(DSD_SET_WIFI_RADIO_REQ, r, (unsigned)o);
}

static void send_wlan_available(const struct wlan_info *w)
{
	uint8_t r[160];
	int o = 0;
	uint8_t u1[4] = { 1, 0, 0, 0 };
	uint8_t t13[4] = { 1, 2, 0, 10 };   /* observed constant on the stock report */
	uint8_t u2[4] = { 2, 0, 0, 0 };
	uint8_t u3[4] = { 2, 0, 0, 0 };
	uint8_t u4[1] = { 1 };
	uint8_t fr[2] = { (uint8_t)(w->freq_mhz & 0xff), (uint8_t)(w->freq_mhz >> 8) };
	uint8_t ssidb[33];
	int sl = w->ssid_len > 0 ? w->ssid_len : 0;

	o = put_tlv(r, o, 0x01, w->bssid, 6);
	o = put_tlv(r, o, 0x10, w->ip_le, 4);
	o = put_tlv(r, o, 0x12, u1, 4);
	o = put_tlv(r, o, 0x13, t13, 4);
	ssidb[0] = (uint8_t)sl;
	memcpy(ssidb + 1, w->ssid, sl);
	o = put_tlv(r, o, 0x1b, ssidb, (uint16_t)(1 + sl));
	o = put_tlv(r, o, 0x1c, fr, 2);
	o = put_tlv(r, o, 0x1f, u2, 4);
	o = put_tlv(r, o, 0x21, u3, 4);
	o = put_tlv(r, o, 0x24, u4, 1);
	dsd_send(DSD_WLAN_AVAILABLE_REQ, r, (unsigned)o);
}

static void send_wlan_meas(const struct wlan_info *w, uint32_t seq)
{
	uint8_t r[160];
	int o = 0;
	uint8_t prof[4] = { WQE_PROFILE_ID, 0, 0, 0 };
	uint8_t sq[4] = { (uint8_t)seq, (uint8_t)(seq >> 8),
	                  (uint8_t)(seq >> 16), (uint8_t)(seq >> 24) };
	uint8_t z4[4] = { 0, 0, 0, 0 };
	uint8_t one1[1] = { 1 };
	uint8_t ssidb[33];
	int sl = w->ssid_len > 0 ? w->ssid_len : 0;
	uint8_t m[43];
	int16_t rssi = REPORT_RSSI;
	int i = 0;

	o = put_tlv(r, o, 0x01, prof, 4);
	o = put_tlv(r, o, 0x02, sq, 4);
	o = put_tlv(r, o, 0x03, z4, 4);
	o = put_tlv(r, o, 0x04, one1, 1);
	ssidb[0] = (uint8_t)sl;
	memcpy(ssidb + 1, w->ssid, sl);
	o = put_tlv(r, o, 0x05, ssidb, (uint16_t)(1 + sl));

	/* TLV 0x10 (43 bytes): 01 | BSSID[6] | freq[2] | 0000 0000 | 01 |
	 *                      00*7 | rssi[2, signed LE] | 00*20   */
	memset(m, 0, sizeof(m));
	m[i++] = 0x01;
	memcpy(m + i, w->bssid, 6); i += 6;
	m[i++] = (uint8_t)(w->freq_mhz & 0xff);
	m[i++] = (uint8_t)(w->freq_mhz >> 8);
	i += 4;              /* 4 zero bytes */
	m[i++] = 0x01;
	i += 7;              /* 7 zero bytes */
	m[i++] = (uint8_t)(rssi & 0xff);
	m[i++] = (uint8_t)((rssi >> 8) & 0xff);
	/* remaining bytes already zero, total length 43 */
	o = put_tlv(r, o, 0x10, m, 43);
	dsd_send(DSD_WLAN_MEAS_REQ, r, (unsigned)o);
}

int main(void)
{
	if (!qmux_enabled()) {
		__system_property_set(STATUS_PROP, "off");
		return 0;
	}
	if (resolve_syms()) {
		__system_property_set(STATUS_PROP, "error");
		return 0;
	}

	qmi_idl_service_object_type so = NULL;
	for (int m = 0; m <= 300 && !so; m++)
		so = p_dsd_get_so(1, m, 6);
	if (!so) {
		LOGE("no dsd service object");
		__system_property_set(STATUS_PROP, "error");
		return 0;
	}

	char osbuf[512];
	uint32_t seq = 1;
	const char *status = NULL;           /* current STATUS_PROP value */

	enum { ST_SEARCH, ST_PRIME, ST_UP } state = ST_SEARCH;
	int prime_left = 0;                  /* feed cycles remaining in this prime */
	uint8_t cur_bssid[6] = { 0 };        /* BSSID we last primed for (0 = none) */
	int up_elapsed = 0;                  /* seconds in ST_UP, for the re-prime timer */

	for (;;) {
		int nap = FAST_SEC;

		if (!g_c) {
			memset(osbuf, 0, sizeof(osbuf));
			if (p_init_instance(so, QMI_CLIENT_INSTANCE_ANY, ind_cb, NULL,
			                    osbuf, QMI_TIMEOUT_MS, &g_c) || !g_c) {
				g_c = NULL;
				set_status("searching", &status);
				sleep(FAST_SEC);
				continue;
			}
		}

		struct wlan_info w;
		int assoc = (read_wlan(&w) == 0 && w.connected);

		if (!assoc) {
			/* Dropped: reset so a reconnect (even to the same AP) re-primes. */
			if (state != ST_SEARCH)
				LOGI("wlan0 not associated; idle");
			state = ST_SEARCH;
			memset(cur_bssid, 0, 6);
			set_status("searching", &status);
			sleep(FAST_SEC);
			continue;
		}

		/* Associated. A new/changed BSSID means a fresh association to prime. */
		if (memcmp(cur_bssid, w.bssid, 6) != 0) {
			memcpy(cur_bssid, w.bssid, 6);
			state = ST_PRIME;
			prime_left = PRIME_CYCLES;
			LOGI("priming modem: ssid=\"%s\" freq=%uMHz", w.ssid, w.freq_mhz);
		}

		if (state == ST_PRIME) {
			send_wifi_switch();
			send_wlan_available(&w);
			send_wlan_meas(&w, seq++);
			set_status("feeding", &status);
			if (--prime_left <= 0) {
				state = ST_UP;
				up_elapsed = 0;
				LOGI("IWLAN primed; backing off to idle watch");
			}
			nap = FAST_SEC;
		} else { /* ST_UP: IWLAN self-maintains; just watch cheaply. */
			set_status("up", &status);
			up_elapsed += SLOW_SEC;
			if (up_elapsed >= REPRIME_SEC) {   /* periodic insurance re-prime */
				up_elapsed = 0;
				state = ST_PRIME;
				prime_left = 3;
			}
			nap = SLOW_SEC;
		}
		sleep(nap);
	}
}
