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
 * and only re-primes on a disassociation/roam.
 *
 * The DOWN edge is just as load-bearing (memory nas-slow-reattach part 9,
 * 2026-08-23): the modem holds WLAN-available indefinitely once reported, and
 * a stale WLAN entry makes its IMS RegisterManager go IWLAN-first on every
 * cold radio power-up — chasing a registration that cannot succeed for ~45 s
 * before falling back to LTE = the ~50 s "Emergency calls only" wedge after
 * any RF drop (bench A/B: cold re-attach 50 s stale vs 4 s cleared). So:
 *   - on wlan0 disassociation the daemon sends DSD 0x21 WLAN_NOT_AVAILABLE;
 *   - at startup it assumes possibly-stale state and clears it if wlan0 is
 *     not associated (covers a previous boot/run having left it armed);
 *   - "wfc_wlan_bridge down" runs the same clear as a oneshot, used by init
 *     when the user turns WFC off (init stops the daemon, so its own down
 *     edge can never fire) and at boot while WFC is off (see init.qmux.rc).
 * Whether the daemon runs at all is gated by
 * init on the user's Wi-Fi-calling toggle (see init.qmux.rc + WfcBridge, which
 * mirrors the Settings toggle to persist.sys.pepito.wfc_enabled).
 *
 * "Wi-Fi preferred" (memory wfc-wifi-preference-lane, bench 2026-10-15): the
 * modem's in-call IWLAN->LTE rule is `Wifi < D && LTE > LTE2` (its own F3,
 * qpDplHandOver.c:4291). Nothing on this stack ever arms the modem's WQE
 * profile, so its Wi-Fi quality is permanently INVALID (= "< D") — the 0x3c
 * RSSI we send below lands in ANDSF's "wqe profile type is inactive" bin —
 * which leaves `LTE > LTE2 (-115)` as the only live term: every call that
 * starts on Wi-Fi is handed to LTE ~1 s after answer wherever LTE is above
 * -115 dBm. The lever is IMSS SET_HANDOVER_CONFIG (0x5d) LTE threshold 2
 * (SET TLV 0x16, read back as GET 0x5e TLV 0x17): at -80 the call STAYS on
 * Wi-Fi. But a static -80 also blocks the modem's `None->LTE` rescue, so a
 * call drops ~20 s after Wi-Fi vanishes (WIFI_LOST, proven). Hence this
 * daemon steers it dynamically, the way a stock AP-side CnE would:
 *   TH2 = -80  while WFC is on, the user chose Wi-Fi preferred, wlan0 is
 *              associated, its RSSI is decent AND the framework has validated
 *              the Wi-Fi network (vendor.qmux.wfc_wifi_ok, mirrored by
 *              WfcBridge from ConnectivityManager NET_CAPABILITY_VALIDATED) —
 *              an AP whose uplink is dead must not hold the LTE rescue off;
 *   TH2 = -115 (stock) the moment any of that stops being true.
 * Proven: Wi-Fi dropped mid-call, TH2 restored 1 s later -> modem handed the
 * call to LTE 5.4 s after the write, call survived. The down edge is the
 * time-critical path (20 s media timeout), so while TH2 is held sticky wlan0
 * is polled every STICKY_SEC (ioctls only, no QMI). The modem range-checks
 * the value: anything above about -70 is silently ignored (result=0, readback
 * unchanged), -80 is accepted. The store is EFS-backed, so a value left at
 * -80 by a previous boot is put back to stock on the next start.
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
#include <time.h>
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
/* Public mirror of the status (vendor_pepito_modem_prop) so the Modem tweaks
 * screen (LineageParts, a coredomain app) can show the feeder state — it may
 * never read the vendor-internal vendor.qmux.* status prop above. */
#define PUBLIC_STATUS_PROP "vendor.pepito.modem.wfc"
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
#define DSD_WLAN_NOT_AVAILABLE_REQ 0x0021
#define DSD_WLAN_MEAS_REQ      0x003c
#define DOWN_RETRIES    40      /* "down" oneshot: wait up to ~2 min for DSD */
#define WQE_PROFILE_ID         1       /* the armed WQE profile on pepito; 3/6 -> err 22 */
#define REPORT_RSSI            (-50)   /* strong. Handover is NOT RSSI-driven (part 41); */
                                       /* a plausible good value keeps the modem from roving. */

/* IMSS (ims_settings, svc 0x12) handover config — the Wi-Fi-preferred lever. */
#define IMSS_SET_HANDOVER_CONFIG 0x005d
#define IMSS_GET_HANDOVER_CONFIG 0x005e
#define IMSS_TH2_SET_TLV        0x16   /* SET 0x5d: LTE threshold 2, int32 dBm */
#define IMSS_TH2_GET_TLV        0x17   /* GET 0x5e: same field (ids are SET+1) */
#define TH2_STOCK               (-115) /* every unit we have read; also the fallback */
#define TH2_STICKY              (-80)  /* highest value the modem accepts (-70+ rejected) */
#define RSSI_HOLD               (-75)  /* arm sticky at/above this wlan0 RSSI ... */
#define RSSI_RELEASE            (-82)  /* ... release below this (hysteresis) */
#define RSSI_UNKNOWN            (-127)
#define STICKY_SEC              2      /* wlan0 poll while TH2 is held sticky (no QMI) */
/* The user's Settings "Calling preference", mirrored by WfcBridge into
 * persist.sys.pepito.wfc_mode and by init.qmux.rc into this vendor prop.
 * Values = ImsMmTelManager.WIFI_MODE_*: 0 wifi-only, 1 cellular-pref, 2 wifi-pref. */
#define WFC_MODE_PROP           "vendor.qmux.wfc_mode"
/* Framework's verdict on the Wi-Fi network (ConnectivityManager VALIDATED),
 * mirrored by WfcBridge into sys.pepito.wfc_wifi_ok and by init.qmux.rc into
 * this vendor prop. Association + RSSI say the LINK is fine; this says the
 * internet behind it is. Unset = not ok (a dead mirror degrades to stock). */
#define WFC_WIFI_OK_PROP        "vendor.qmux.wfc_wifi_ok"
#define TH2_STATUS_PROP         "vendor.qmux.wfc_th2"   /* stock | sticky | error */

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
static qmi_idl_service_object_type (*p_imss_get_so)(int, int, int);

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

/* The user's Wi-Fi-calling toggle, mirrored by init.qmux.rc into a vendor prop
 * this (vendor) domain is already allowed to read (vendor_qmux_prop); the
 * Settings-owned persist.sys.pepito.wfc_enabled is not vendor-readable. The
 * feeder now runs for the life of the boot so SOMETHING always clears a stale
 * modem WLAN-available entry on wlan0-down (the IWLAN-first "emergency calls
 * only" wedge), but only FEEDS the modem when WFC is on. Unset = off. */
#define WFC_ON_PROP "vendor.qmux.wfc_on"
static int wfc_on(void)
{
	char v[PROP_VALUE_MAX] = {0};
	if (__system_property_get(WFC_ON_PROP, v) > 0)
		return strcmp(v, "1") == 0;
	return 0;
}

/* Wi-Fi preferred chosen by the user (wifi-only counts too). Unset = the
 * carrier default = cellular preferred, i.e. stock behaviour. */
static int wifi_preferred(void)
{
	char v[PROP_VALUE_MAX] = {0};
	if (__system_property_get(WFC_MODE_PROP, v) > 0)
		return strcmp(v, "2") == 0 || strcmp(v, "0") == 0;
	return 0;
}

static int wifi_ok(void)
{
	char v[PROP_VALUE_MAX] = {0};
	if (__system_property_get(WFC_WIFI_OK_PROP, v) > 0)
		return strcmp(v, "1") == 0;
	return 0;
}

/* Block until the WFC toggle changes (zero wakeups) with a safety re-check
 * timeout. When WFC is off nothing can arm WLAN (only this feeder does, and
 * only when on), so once cleared there is nothing to watch — we just wait for
 * WFC to come back. A short timeout is used while a clear is still pending so
 * it is retried promptly. */
static void wait_wfc_change(int timeout_sec)
{
	const prop_info *pi = __system_property_find(WFC_ON_PROP);
	struct timespec ts = { .tv_sec = timeout_sec, .tv_nsec = 0 };
	if (!pi) { sleep(timeout_sec > 0 ? timeout_sec : 60); return; }
	uint32_t serial = __system_property_serial(pi);
	__system_property_wait(pi, serial, &serial, timeout_sec > 0 ? &ts : NULL);
}

/* Set a status prop only on change (avoids property-set spam each poll). */
static void set_status_prop(const char *prop, const char *s, const char **cur)
{
	if (*cur && strcmp(*cur, s) == 0) return;
	__system_property_set(prop, s);
	*cur = s;
}

static void set_status(const char *s, const char **cur)
{
	if (*cur && strcmp(*cur, s) == 0) return;
	__system_property_set(STATUS_PROP, s);
	__system_property_set(PUBLIC_STATUS_PROP, s);
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
	p_imss_get_so = (qmi_idl_service_object_type (*)(int, int, int))
	                dlsym(svc, "imss_get_service_object_internal_v01");
	if (!p_init_instance || !p_send_raw || !p_release || !p_dsd_get_so ||
	    !p_imss_get_so) {
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
	int      rssi;       /* dBm from SIOCGIWSTATS, RSSI_UNKNOWN if not reported */
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
	w->rssi = RSSI_UNKNOWN;
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

	/* Link quality: with IW_QUAL_DBM the level byte is signed dBm (what
	 * wcn/pronto reports); otherwise it is a driver-relative number we treat
	 * as unknown rather than guess. */
	struct iw_statistics st;
	memset(&wrq, 0, sizeof(wrq));
	memset(&st, 0, sizeof(st));
	strncpy(wrq.ifr_ifrn.ifrn_name, IFACE, IFNAMSIZ - 1);
	wrq.u.data.pointer = &st;
	wrq.u.data.length = sizeof(st);
	if (ioctl(s, SIOCGIWSTATS, &wrq) == 0 &&
	    !(st.qual.updated & IW_QUAL_LEVEL_INVALID) &&
	    (st.qual.updated & IW_QUAL_DBM))
		w->rssi = (int8_t)st.qual.level;

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

/* LTE threshold 2 cache (see th2_assert): 1 once g_th2 reflects the modem. A
 * DSD transport error is our tell for a modem restart, after which the modem
 * may hold whatever EFS last flushed rather than what we last wrote. */
static int g_th2_known = 0;
static int g_th2;

/* Returns QMI result (0 = success), or a negative transport error. */
static int dsd_send(unsigned int msg, uint8_t *req, unsigned int len)
{
	uint8_t resp[2048];
	unsigned int got = 0;
	int rc = p_send_raw(g_c, msg, req, len, resp, sizeof(resp), &got,
	                    QMI_TIMEOUT_MS);
	if (rc) { g_th2_known = 0; return -1; }
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

/* WLAN_NOT_AVAILABLE carries no TLVs. Returns 0 only on QMI success, so the
 * caller can retry on a transport error or modem-side failure. */
static int send_wlan_unavailable(void)
{
	return dsd_send(DSD_WLAN_NOT_AVAILABLE_REQ, NULL, 0);
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

/* ---- IMSS handover-config steering (LTE threshold 2) ---- */
static qmi_client_type g_imss;
static qmi_idl_service_object_type g_imss_so;
static char g_imss_os[512];

static int imss_connect(void)
{
	if (g_imss) return 0;
	if (!g_imss_so)
		for (int m = 0; m <= 300 && !g_imss_so; m++)
			g_imss_so = p_imss_get_so(1, m, 6);
	if (!g_imss_so) return -1;
	memset(g_imss_os, 0, sizeof(g_imss_os));
	if (p_init_instance(g_imss_so, QMI_CLIENT_INSTANCE_ANY, ind_cb, NULL,
	                    g_imss_os, QMI_TIMEOUT_MS, &g_imss) || !g_imss) {
		g_imss = NULL;
		return -1;
	}
	return 0;
}

static void imss_drop(void)
{
	if (g_imss) p_release(g_imss);
	g_imss = NULL;
}

/* GET 0x5e and pull one u32 TLV. 0 on success. */
static int imss_get_u32(unsigned int msg, uint8_t tlv, uint32_t *val)
{
	uint8_t resp[2048];
	unsigned int got = 0;
	if (imss_connect()) return -1;
	if (p_send_raw(g_imss, msg, NULL, 0, resp, sizeof(resp), &got,
	               QMI_TIMEOUT_MS)) { imss_drop(); return -1; }
	const uint8_t *p = resp, *e = resp + got;
	while (p + 3 <= e) {
		uint8_t t = p[0];
		uint16_t l = (uint16_t)(p[1] | (p[2] << 8));
		const uint8_t *v = p + 3;
		if (v + l > e) break;
		if (t == tlv && l == 4) {
			*val = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
			       ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
			return 0;
		}
		p = v + l;
	}
	return -1;
}

/* SET with one u32 TLV; returns the QMI result (0 = success) or -1. */
static int imss_set_u32(unsigned int msg, uint8_t tlv, uint32_t val)
{
	uint8_t req[7] = { tlv, 4, 0, (uint8_t)val, (uint8_t)(val >> 8),
	                   (uint8_t)(val >> 16), (uint8_t)(val >> 24) };
	uint8_t resp[2048];
	unsigned int got = 0;
	if (imss_connect()) return -1;
	if (p_send_raw(g_imss, msg, req, sizeof(req), resp, sizeof(resp), &got,
	               QMI_TIMEOUT_MS)) { imss_drop(); return -1; }
	const uint8_t *p = resp, *e = resp + got;
	while (p + 3 <= e) {
		uint8_t t = p[0];
		uint16_t l = (uint16_t)(p[1] | (p[2] << 8));
		const uint8_t *v = p + 3;
		if (v + l > e) break;
		if (t == 0x02 && l >= 4) return (v[0] | (v[1] << 8));
		p = v + l;
	}
	return 0;
}

/* Assert LTE threshold 2 = `want` (dBm). Read-compare-write, read back, so
 * a wrong/ignored write is visible. Keeps the last verified value so the
 * normal poll sends nothing at all, and a known-but-different value goes
 * straight to SET (the Wi-Fi-loss down edge is time-critical: one round trip
 * fewer inside the modem's ~20 s media timeout). The cache is dropped on any
 * DSD transport error and once per re-prime period (main loop), so a modem
 * restart or an unflushed EFS write cannot leave a stale belief for the boot. */
static void th2_assert(int want, const char **th2_status)
{
	uint32_t cur;
	int from;
	if (g_th2_known && g_th2 == want) return;
	if (g_th2_known) {
		from = g_th2;
		cur = (uint32_t)(int32_t)from;
	} else {
		if (imss_get_u32(IMSS_GET_HANDOVER_CONFIG, IMSS_TH2_GET_TLV, &cur)) {
			set_status_prop(TH2_STATUS_PROP, "error", th2_status);
			return;
		}
		from = (int32_t)cur;
	}
	if ((int32_t)cur != want) {
		int rc = imss_set_u32(IMSS_SET_HANDOVER_CONFIG, IMSS_TH2_SET_TLV,
		                      (uint32_t)(int32_t)want);
		if (rc || imss_get_u32(IMSS_GET_HANDOVER_CONFIG, IMSS_TH2_GET_TLV, &cur)
		    || (int32_t)cur != want) {
			LOGE("LTE TH2 %d -> %d: rc=%d readback=%d", from, want, rc,
			     (int32_t)cur);
			g_th2_known = 0;
			set_status_prop(TH2_STATUS_PROP, "error", th2_status);
			return;
		}
		LOGI("LTE TH2 -> %d (%s)", want,
		     want == TH2_STICKY ? "Wi-Fi preferred, hold call on Wi-Fi"
		                        : "stock, LTE rescue armed");
	}
	g_th2 = want;
	g_th2_known = 1;
	set_status_prop(TH2_STATUS_PROP, want == TH2_STICKY ? "sticky" : "stock",
	                th2_status);
}

int main(int argc, char **argv)
{
	int down_mode = argc > 1 && strcmp(argv[1], "down") == 0;

	if (!qmux_enabled()) {
		if (!down_mode)
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

	/* "down" oneshot: send a single WLAN_NOT_AVAILABLE and exit. Used by init
	 * when the user turns WFC off (which stops the persistent daemon before
	 * its down edge could run) and at boot while WFC is off, so no stale
	 * WLAN-available entry survives to wedge cold IMS re-attach. QCCI may run
	 * before the modem's DSD service exists at boot — retry briefly. */
	if (down_mode) {
		for (int t = 0; t < DOWN_RETRIES; t++) {
			memset(osbuf, 0, sizeof(osbuf));
			if (!p_init_instance(so, QMI_CLIENT_INSTANCE_ANY, ind_cb, NULL,
			                     osbuf, QMI_TIMEOUT_MS, &g_c) && g_c) {
				int rc = send_wlan_unavailable();
				LOGI("down: WLAN_NOT_AVAILABLE rc=%d", rc);
				p_release(g_c);
				if (rc == 0)
					return 0;
				g_c = NULL;
			}
			sleep(FAST_SEC);
		}
		LOGE("down: DSD service never accepted the clear");
		return 1;
	}

	uint32_t seq = 1;
	const char *status = NULL;           /* current STATUS_PROP value */
	const char *th2_status = NULL;       /* current TH2_STATUS_PROP value */
	int sticky = 0;                      /* LTE TH2 currently held at TH2_STICKY */
	int rssi_warned = 0;

	enum { ST_SEARCH, ST_PRIME, ST_UP } state = ST_SEARCH;
	int prime_left = 0;                  /* feed cycles remaining in this prime */
	uint8_t cur_bssid[6] = { 0 };        /* BSSID we last primed for (0 = none) */
	int up_elapsed = 0;                  /* seconds in ST_UP, for the re-prime timer */
	int wlan_reported = 1;               /* modem may hold WLAN-available; starts 1 so
	                                        a boot with Wi-Fi down clears stale state
	                                        left by a previous boot/run */

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

		/* WFC off: never feed/arm. Make sure no stale WLAN-available is left to
		 * wedge cold IMS re-attach (the gap the old WFC-gated START left — with
		 * the feeder stopped, nothing ran to clear it), then block on the toggle
		 * with no wakeups. This is what lets the daemon run for the whole boot
		 * yet burn no feeding power when WFC is off. */
		if (!wfc_on()) {
			if (wlan_reported && send_wlan_unavailable() == 0) {
				wlan_reported = 0;
				LOGI("WFC off: cleared WLAN availability");
			}
			state = ST_SEARCH;
			memset(cur_bssid, 0, 6);
			/* WFC off = stock handover thresholds, always (also puts back a
			 * sticky value a previous boot left in EFS). */
			th2_assert(TH2_STOCK, &th2_status);
			sticky = 0;
			set_status("off", &status);
			wait_wfc_change((wlan_reported || !g_th2_known) ? 10 : 300);
			continue;
		}

		struct wlan_info w;
		int assoc = (read_wlan(&w) == 0 && w.connected);

		if (!assoc) {
			/* Dropped: reset so a reconnect (even to the same AP) re-primes. */
			if (state != ST_SEARCH)
				LOGI("wlan0 not associated; idle");
			state = ST_SEARCH;
			memset(cur_bssid, 0, 6);
			/* Down edge, time-critical half: give the modem its LTE rescue
			 * back BEFORE anything else. A call still up on the dead Wi-Fi
			 * leg is dropped ~20 s after the loss unless TH2 is stock; the
			 * modem hands it to LTE ~5 s after this write (proven). */
			th2_assert(TH2_STOCK, &th2_status);   /* boot-time assert too */
			sticky = 0;
			/* Down edge: the modem never ages WLAN-available out on its own,
			 * and a stale entry sends cold IMS re-attach down the ~45 s
			 * IWLAN-first path ("Emergency calls only" wedge). Clear it; keep
			 * the flag on failure so the next poll retries. */
			if (wlan_reported && send_wlan_unavailable() == 0) {
				wlan_reported = 0;
				LOGI("wlan0 down: sent WLAN_NOT_AVAILABLE");
			}
			set_status("searching", &status);
			sleep(FAST_SEC);
			continue;
		}

		/* Associated: steer LTE TH2. Sticky only for Wi-Fi preferred, a
		 * decent link AND a framework-validated Wi-Fi network (an AP with a
		 * dead uplink looks perfect from here but must not hold the LTE
		 * rescue off), with hysteresis so a wobbling RSSI does not flap the
		 * modem; an unknown RSSI (driver not reporting dBm) never arms it. */
		int pref = wifi_preferred();
		int ok = wifi_ok();
		if (w.rssi == RSSI_UNKNOWN && pref && !rssi_warned) {
			LOGE("wlan0 reports no dBm RSSI; Wi-Fi preferred cannot be held");
			rssi_warned = 1;
		}
		int want_sticky = pref && ok && w.rssi != RSSI_UNKNOWN &&
		                  (sticky ? w.rssi >= RSSI_RELEASE : w.rssi >= RSSI_HOLD);
		if (want_sticky != sticky)
			LOGI("wlan0 rssi %d dBm, Wi-Fi preferred %d, validated %d -> TH2 %s",
			     w.rssi, pref, ok, want_sticky ? "sticky" : "stock");
		th2_assert(want_sticky ? TH2_STICKY : TH2_STOCK, &th2_status);
		sticky = want_sticky && g_th2_known && g_th2 == TH2_STICKY;

		/* A new/changed BSSID means a fresh association to prime. */
		if (memcmp(cur_bssid, w.bssid, 6) != 0) {
			memcpy(cur_bssid, w.bssid, 6);
			state = ST_PRIME;
			prime_left = PRIME_CYCLES;
			wlan_reported = 1;
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
			nap = SLOW_SEC;
		}
		/* While TH2 is sticky the wlan0 watch must be quick (ioctls only,
		 * no QMI unless something changes): the Wi-Fi-loss edge has to reach
		 * the modem well inside its ~20 s in-call media timeout. Only once
		 * primed: PRIME_CYCLES counts loop turns, so clamping during a prime
		 * would cut the ~75 s feed to ~50 s and send every 2 s. */
		if (state == ST_UP) {
			if (sticky && nap > STICKY_SEC)
				nap = STICKY_SEC;
			up_elapsed += nap;
			if (up_elapsed >= REPRIME_SEC) {   /* periodic insurance re-prime */
				up_elapsed = 0;
				state = ST_PRIME;
				prime_left = 3;
				g_th2_known = 0;   /* and re-read TH2 from the modem */
			}
		}
		sleep(nap);
	}
}
