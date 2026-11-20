// ota.h - Over-the-air firmware update for Gchess.
//
// Include it LAST in Gchess.ino (after net.h, pages.h and setup.h): it uses things from all of them.
//
//   NET SIDE (core 1, only inside netTask):  manifest download, version list, firmware download + verify
//   UI  SIDE (core 0, only inside uiTask):   dialogs, progress page, About "Update" list, power-off hook
//   The two sides talk ONLY through events.h (NC_OTA_CHECK, NC_OTA_START, UE_OTA, LK_OTA / UE_LIST).
//
// UE_OTA fields:  a = OtaState,  b = percent (PROGRESS) | manifest index (OFFER, INDEX) | 1 = manual (UPTODATE)
//                 s1 = version,  s2 = short text (notes / status / error)
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <new>
#include "gchess_config.h"
#include "events.h"

// ---------------------------------------------------------------------------------------------
// Tunables (all can be overridden in gchess_config.h). MANIFEST_URL and GC_HW MUST be defined there.
// ---------------------------------------------------------------------------------------------
#ifndef OTA_MAX_ENTRIES
#define OTA_MAX_ENTRIES 10          // manifest entries kept in RAM (only hw == GC_HW)
#endif
#ifndef OTA_LATEST_MAX
#define OTA_LATEST_MAX 2048         // latest.json is tiny (one entry per hardware line)
#endif
#ifndef OTA_MANIFEST_MAX
#define OTA_MANIFEST_MAX 4096       // full list buffer (about 11 versions); taken before TLS starts, so keep it small
#endif
#ifndef OTA_TIMEOUT_MS
#define OTA_TIMEOUT_MS 15000        // connect / handshake / response-header timeout
#endif
#ifndef OTA_STALL_MS
#define OTA_STALL_MS 12000          // no download bytes for this long -> abort
#endif
#ifndef OTA_TOTAL_MS
#define OTA_TOTAL_MS 600000UL       // whole download must finish within 10 minutes
#endif
#ifndef OTA_CONNECT_MS
#define OTA_CONNECT_MS 8000         // one TCP connect attempt (GitHub from some networks times out now and then)
#endif
#ifndef OTA_CONNECT_TRIES
#define OTA_CONNECT_TRIES 4         // attempt 0 = normal DNS name, 1..3 = the other GitHub raw addresses 185.199.109/110/111.133
#endif
#ifndef OTA_GIVEUP_MS
#define OTA_GIVEUP_MS 60000UL       // no new bytes for this long (all reconnects included) -> give up and keep the old firmware
#endif
#ifndef OTA_SLIM_BOOT
#define OTA_SLIM_BOOT 1             // 1 = an update restarts into a slim boot (no game/UI/net tasks, ~100 KB more free heap), 0 = update while running
#endif
#ifndef OTA_MAX_RESUMES
#define OTA_MAX_RESUMES 40          // reconnect + continue (HTTP Range) this many times before giving up
#endif
#ifndef OTA_MAX_REDIRECTS
#define OTA_MAX_REDIRECTS 4
#endif
// Measured on the real board: only ~63 KB of heap is free even with NO socket open, and the Lichess TLS
// connection works at that level. So the limits are low; if the TLS connect really runs out of memory the
// download fails safely ("Low memory") and the running firmware stays untouched.
#ifndef OTA_CHECK_MIN_HEAP
#define OTA_CHECK_MIN_HEAP 50000    // free heap needed for the small latest.json / manifest check
#endif
#ifndef OTA_MIN_HEAP
#define OTA_MIN_HEAP 52000          // free heap needed to start a download
#endif
#ifndef OTA_MIN_BLOCK
#define OTA_MIN_BLOCK 20000         // largest free block needed (TLS buffers are contiguous allocations)
#endif
#ifndef OTA_VALID_AFTER_MS
#define OTA_VALID_AFTER_MS 10000    // Wi-Fi connected this long -> new firmware is marked valid
#endif
#ifndef OTA_MANIFEST_TRIES
#define OTA_MANIFEST_TRIES 3        // automatic manifest attempts per Wi-Fi connect
#endif
#ifndef LATEST_URL
#define LATEST_URL MANIFEST_URL     // small file with only the newest versions (automatic check); falls back to the full list
#endif
#define OTA_MAX_BIN 0x1F0000UL      // sanity limit for "size" in the manifest (slot is 0x1E0000)

// Value your dialog callback receives for button 1 / 2 / 3 (1-based, like powerDlgCb in ui.h).
enum { OTA_B1 = 1, OTA_B2 = 2, OTA_B3 = 3 };   // my dialog calls cb(1|2|3)

// ---------------------------------------------------------------------------------------------
// Rollback support.  Arduino core 3.x calls esp_ota_mark_app_valid_cancel_rollback() by itself at boot
// unless this function returns true.  We return true and validate later (otaNetTick: Wi-Fi + 10 s).
// ---------------------------------------------------------------------------------------------
extern "C" bool verifyRollbackLater() { return true; }

// ---------------------------------------------------------------------------------------------
// Power gate.  Battery / charger hardware is not built yet: always OK for now.
// ---------------------------------------------------------------------------------------------
inline bool otaPowerOk() {
    return true;
    // When the hardware exists, replace the line above with the real rule:
    // return batteryPercent() >= 50 || isCharging();
}

// ---------------------------------------------------------------------------------------------
// Version helpers: "X.Y.Z" (X structure/hardware, Y feature, Z bugfix), compared NUMERICALLY.
// ---------------------------------------------------------------------------------------------
static bool otaVerParse(const char* s, uint32_t* out) {
    uint32_t v[3];
    const char* p = s;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*p)) return false;
        uint32_t n = 0;
        int d = 0;
        while (isdigit((unsigned char)*p)) {
            n = n * 10 + (uint32_t)(*p - '0');
            if (++d > 3) return false;
            p++;
        }
        v[i] = n;
        if (i < 2) {
            if (*p != '.') return false;
            p++;
        }
    }
    if (*p) return false;
    if (out) *out = v[0] * 1000000UL + v[1] * 1000UL + v[2];
    return true;
}

// -1 / 0 / +1.  A string that does not parse counts as 0.0.0.
static int otaVerCmp(const char* a, const char* b) {
    uint32_t x = 0, y = 0;
    otaVerParse(a, &x);
    otaVerParse(b, &y);
    return x < y ? -1 : (x > y ? 1 : 0);
}

// ---------------------------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------------------------
struct OtaEntry {
    char ver[12];
    char url[160];     // never leaves this file (not put into events)
    char sha[65];      // 64 hex chars, lower case
    uint32_t size;
    char note[32];
};
static OtaEntry gOtaMan[OTA_MAX_ENTRIES];   // newest first.  Owned by the net task.
static uint8_t gOtaN = 0;

static char gOtaSkip[12] = "";              // written by the UI task (and NVS), read by the net task
static bool gOtaPend = false;               // mirror of NVS "ota_pend"

// ============================================================================================
//                                       NET SIDE  (core 1)
// ============================================================================================
static char gOtaLastErr[40] = "";           // last failure text (used by the slim update boot)
static volatile bool gOtaInstalled = false;  // set when Update.end() succeeded
static volatile bool gOtaBusy = false;      // a download is running: netTask skips all polling (see hook)
static bool gOtaNeedValid = false;          // running image is PENDING_VERIFY
static uint32_t gOtaWifiUpAt = 0;           // millis() of the Wi-Fi connect edge, 0 = not connected
static bool gOtaWasConn = false;
static bool gOtaManifestDue = false;
static bool gOtaOfferDue = false;
static uint32_t gOtaNextTry = 0;
static uint8_t gOtaTries = 0;

// The ONLY place where certificate checking is switched off (same trade-off as net.h).
static void otaTlsSetup(WiFiClientSecure& c) {
    c.setInsecure();
    c.setHandshakeTimeout(OTA_TIMEOUT_MS / 1000);
}

// One HTTPS connection (client + HTTPClient on the heap, freed by close()).
struct OtaConn {
    WiFiClientSecure* cl = nullptr;
    HTTPClient* http = nullptr;
    bool preFailed = false;                                    // explicit-IP connect failed: do not call GET
    bool open(const char* url, int attempt = 0) {
        preFailed = false;
        cl = new (std::nothrow) WiFiClientSecure();
        http = new (std::nothrow) HTTPClient();
        if (!cl || !http) {
            close();
            return false;
        }
        otaTlsSetup(*cl);
        cl->setTimeout(OTA_TIMEOUT_MS / 1000);
        http->setConnectTimeout(OTA_CONNECT_MS);
        http->setTimeout(OTA_TIMEOUT_MS);
        http->setReuse(false);
        http->setUserAgent("Gchess/" GC_VERSION);
        http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // raw.githubusercontent.com / release assets
        http->setRedirectLimit(OTA_MAX_REDIRECTS);                 // redirect loop -> non-200 -> error
        if (!http->begin(*cl, url)) {
            close();
            return false;
        }
        // Retry with another GitHub address: connect first, HTTPClient then reuses the connected socket (Host header stays the real name).
        if (attempt > 0 && attempt < 4 && strstr(url, "://raw.githubusercontent.com/")) {
            IPAddress ip(185, 199, 108 + attempt, 133);
            DBG(NET, "ota: retry %d via %s", attempt, ip.toString().c_str());
            if (!cl->connect(ip, 443, OTA_CONNECT_MS)) preFailed = true;
        }
        return true;
    }
    void close() {
        if (http) {
            http->end();
            delete http;
            http = nullptr;
        }
        if (cl) {
            cl->stop();
            delete cl;
            cl = nullptr;
        }
    }
};

static void otaHttpErr(int code, char* out, size_t n) {
    if (code >= 300 && code < 400) strlcpy(out, "Too many redirects", n);
    else if (code < 0) strlcpy(out, "Connection failed", n);
    else snprintf(out, n, "Server error %d", code);
}

static void otaPostErr(const char* txt, const char* ver = "") { uiPost(UE_OTA, ver, txt, OTA_ERROR, 0); }
static void otaPostCheckFail(const char* txt) { uiPost(UE_OTA, "", txt, OTA_CHECK_FAIL, 0); }

// ---- tiny JSON helpers (flat objects only; no ArduinoJson needed) ----
static bool otaJStr(const char* o, const char* key, char* out, size_t cap, bool truncOk = false) {
    char pat[16];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* k = strstr(o, pat);
    if (!k) return false;
    k += strlen(pat);
    while (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n') k++;
    if (*k != ':') return false;
    k++;
    while (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n') k++;
    if (*k != '"') return false;
    k++;
    size_t n = 0;
    while (*k && *k != '"') {
        char ch = *k++;
        if (ch == '\\' && *k) ch = *k++;           // \/ and \"
        if (n + 1 >= cap) {
            if (!truncOk) return false;
            continue;
        }
        out[n++] = ch;
    }
    if (*k != '"') return false;
    out[n] = 0;
    return true;
}

static bool otaJNum(const char* o, const char* key, uint32_t* out) {
    char pat[16];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* k = strstr(o, pat);
    if (!k) return false;
    k += strlen(pat);
    while (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n') k++;
    if (*k != ':') return false;
    k++;
    while (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n') k++;
    if (!isdigit((unsigned char)*k)) return false;
    *out = (uint32_t)strtoul(k, nullptr, 10);
    return true;
}

// Manifest / latest file: {"versions":[{"v":"1.2.0","hw":"A","url":"https://...","size":1400000,"sha256":"64hex","notes":"short"}, ...]}
// Entries must not contain '{' or '}' inside strings.  Modifies "body".
static bool otaParseManifest(char* body) {
    char* p = strchr(body, '[');
    if (!p) return false;
    uint8_t n = 0;
    while (n < OTA_MAX_ENTRIES) {
        char* o = strchr(p, '{');
        if (!o) break;
        char* c = strchr(o, '}');
        if (!c) break;
        *c = 0;
        p = c + 1;
        OtaEntry e;
        memset(&e, 0, sizeof(e));
        char hw[8];
        if (!otaJStr(o, "hw", hw, sizeof(hw)) || strcmp(hw, GC_HW) != 0) continue;       // other hardware: ignore
        if (!otaJStr(o, "v", e.ver, sizeof(e.ver)) || !otaVerParse(e.ver, nullptr)) continue;
        if (!otaJStr(o, "url", e.url, sizeof(e.url)) || strncmp(e.url, "https://", 8) != 0) continue;
        if (!otaJStr(o, "sha256", e.sha, sizeof(e.sha)) || strlen(e.sha) != 64) continue;
        bool hexOk = true;
        for (int i = 0; i < 64; i++) {
            if (!isxdigit((unsigned char)e.sha[i])) hexOk = false;
            e.sha[i] = (char)tolower((unsigned char)e.sha[i]);
        }
        if (!hexOk) continue;
        if (!otaJNum(o, "size", &e.size) || e.size < 100000UL || e.size > OTA_MAX_BIN) continue;
        otaJStr(o, "notes", e.note, sizeof(e.note), true);
        bool dup = false;
        for (int i = 0; i < n; i++) if (otaVerCmp(gOtaMan[i].ver, e.ver) == 0) dup = true;
        if (dup) continue;
        gOtaMan[n++] = e;
    }
    // newest first (insertion sort, at most 10 entries)
    for (int i = 1; i < n; i++) {
        OtaEntry t = gOtaMan[i];
        int j = i - 1;
        while (j >= 0 && otaVerCmp(gOtaMan[j].ver, t.ver) < 0) {
            gOtaMan[j + 1] = gOtaMan[j];
            j--;
        }
        gOtaMan[j + 1] = t;
    }
    gOtaN = n;
    return true;
}

// Stream that collects at most "cap" bytes (used with HTTPClient::writeToStream, which also copes with chunked bodies).
struct OtaSink : public Stream {
    char* b;
    size_t cap, n = 0;
    bool over = false;
    OtaSink(char* buf, size_t c) : b(buf), cap(c) {}
    size_t write(uint8_t ch) override {
        if (n < cap) {
            b[n++] = (char)ch;
            return 1;
        }
        over = true;
        return 0;
    }
    size_t write(const uint8_t* p, size_t l) override {
        size_t k = (n + l <= cap) ? l : cap - n;
        memcpy(b + n, p, k);
        n += k;
        if (k < l) over = true;
        return k;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};

// HTTPS GET of "url" (LATEST_URL = newest only, MANIFEST_URL = full history) into gOtaMan.  Blocks up to the timeouts.
// Log what DNS says for the host of a URL (a failing connect is often a DNS / routing problem, not memory).
static void otaLogDns(const char* url) {
    const char* h = strstr(url, "://");
    h = h ? h + 3 : url;
    char host[64];
    size_t n = 0;
    while (h[n] && h[n] != '/' && h[n] != ':' && n < sizeof(host) - 1) { host[n] = h[n]; n++; }
    host[n] = 0;
    IPAddress ip;
    uint32_t t0 = millis();
    int r = WiFi.hostByName(host, ip);
    DBG(NET, "ota: DNS %s -> %s (ok=%d, %lu ms, dns server %s, free heap %u)", host, ip.toString().c_str(), r, (unsigned long)(millis() - t0), WiFi.dnsIP().toString().c_str(), (unsigned)ESP.getFreeHeap());
}

static bool otaFetchManifest(const char* url, size_t cap, char* err, size_t en) {
    otaLogDns(url);
    // The body buffer is taken BEFORE the TLS session exists: afterwards the heap has no room left for it.
    char* buf = (char*)malloc(cap + 1);
    if (!buf) {
        strlcpy(err, "Out of memory (buffer)", en);
        return false;
    }
    OtaConn c;
    if (!c.open(url)) {
        free(buf);
        strlcpy(err, "Out of memory (open)", en);
        return false;
    }
    bool ok = false;
    int code = -1;
    for (int k = 0; k < OTA_CONNECT_TRIES; k++) {
        if (k && (c.close(), !c.open(url, k))) break;
        code = c.preFailed ? -1 : c.http->GET();
        DBG(NET, "ota: GET (try %d) -> %d (%s)", k, code, HTTPClient::errorToString(code).c_str());
        if (code != -1) break;
    }
    if (code != 200) {
        otaHttpErr(code, err, en);
    } else if (c.http->getSize() > (int)cap) {
        strlcpy(err, "Manifest too big", en);
    } else {
        {
            OtaSink sink(buf, cap);
            int w = c.http->writeToStream(&sink);
            if (w < 0 || sink.over) {
                strlcpy(err, "Manifest read failed", en);
            } else {
                buf[sink.n] = 0;
                ok = otaParseManifest(buf);
                if (!ok) strlcpy(err, "Bad manifest", en);
            }
        }
    }
    c.close();
    free(buf);
    DBG(NET, "ota: manifest %s (%d entries for hw %s) %s", ok ? "ok" : "FAILED", (int)gOtaN, GC_HW, ok ? "" : err);
    return ok;
}

// Index of the newest version if it is newer than us and not skipped, else -1.
static int otaTopNewer() {
    if (!gOtaN) return -1;
    if (otaVerCmp(gOtaMan[0].ver, GC_VERSION) <= 0) return -1;
    if (strcmp(gOtaMan[0].ver, gOtaSkip) == 0) return -1;
    return 0;
}

// After an AUTOMATIC manifest read: offer or "up to date" (the UI decides when it is idle enough to show it).
static void otaPostAuto() {
    int t = otaTopNewer();
    if (t >= 0) uiPost(UE_OTA, gOtaMan[t].ver, gOtaMan[t].note, OTA_OFFER, t);
    else uiPost(UE_OTA, "", "", OTA_UPTODATE, 0);
}

// NC_OTA_CHECK: user tapped Update on the About page.  Re-read the manifest and fill LK_OTA.
static void otaManualCheck() {
    if (gOtaBusy) return;
    char err[40];
    if (WiFi.status() != WL_CONNECTED) return otaPostCheckFail("No Wi-Fi connection");
    if (anyGameActive()) return otaPostCheckFail("Online game in progress");
    if (ESP.getFreeHeap() < OTA_CHECK_MIN_HEAP) hReq.close();
    if (ESP.getFreeHeap() < OTA_CHECK_MIN_HEAP) return otaPostCheckFail("Low memory");
    if (!otaFetchManifest(MANIFEST_URL, OTA_MANIFEST_MAX, err, sizeof(err))) return otaPostCheckFail(err);
    if (!gOtaN) return otaPostCheckFail("No versions published");

    ListData& L = gList[LK_OTA];
    L.n = 0;
    for (int i = 0; i < gOtaN && i < 24; i++) {
        ListItem& it = L.it[i];
        bool cur = otaVerCmp(gOtaMan[i].ver, GC_VERSION) == 0;
        snprintf(it.main, sizeof(it.main), cur ? "v%s  (current)" : "v%s", gOtaMan[i].ver);
        strlcpy(it.sub, gOtaMan[i].note, sizeof(it.sub));
        strlcpy(it.id, gOtaMan[i].ver, sizeof(it.id));
        it.val = i;
    }
    L.n = gOtaN;
    uiPost(UE_LIST, "", "", LK_OTA, gOtaN);

    int t = otaTopNewer();
    if (t >= 0) uiPost(UE_OTA, gOtaMan[t].ver, gOtaMan[t].note, OTA_INDEX, t);     // silent: only refreshes the UI's index
    else uiPost(UE_OTA, "", "", OTA_UPTODATE, otaVerCmp(gOtaMan[0].ver, GC_VERSION) <= 0 ? 1 : 0);
}

// ---- download ----
struct OtaRun {
    OtaConn conn;
    OtaEntry e;
    uint32_t total = 0, got = 0, lastData = 0, startedAt = 0, lastBytesAt = 0;
    int lastPct = -1;
    bool updBegun = false, shaInit = false;
    uint8_t attempt = 0, resumes = 0;                   // which GitHub address works, how often we reconnected
    mbedtls_sha256_context sha;
};
static OtaRun gOtaR;

static void otaCleanup() {
    gOtaR.conn.close();
    if (gOtaR.shaInit) {
        mbedtls_sha256_free(&gOtaR.sha);
        gOtaR.shaInit = false;
    }
}

// Any failure: keep the current firmware, never reboot, resume polling.
static void otaFail(const char* txt) {
    strlcpy(gOtaLastErr, txt, sizeof(gOtaLastErr));
    if (gOtaR.updBegun) {
        Update.abort();
        gOtaR.updBegun = false;
    }
    otaCleanup();
    gOtaBusy = false;                                   // polling resumes
    DBG(NET, "ota: FAILED: %s (free heap %u)", txt, (unsigned)ESP.getFreeHeap());
    otaPostErr(txt, gOtaR.e.ver);
}

static bool otaShaMatch(const uint8_t* h, const char* hex) {
    for (int i = 0; i < 32; i++) {
        char t[3];
        snprintf(t, sizeof(t), "%02x", h[i]);
        if (tolower((unsigned char)hex[2 * i]) != t[0] || tolower((unsigned char)hex[2 * i + 1]) != t[1]) return false;
    }
    return true;
}

static void otaFinish() {
    OtaRun& r = gOtaR;
    uiPost(UE_OTA, r.e.ver, "Verifying", OTA_PROGRESS, 100);
    uint8_t h[32];
    mbedtls_sha256_finish(&r.sha, h);
    if (!otaShaMatch(h, r.e.sha)) return otaFail("Hash mismatch");     // nothing was activated yet
    r.updBegun = false;                                                // Update.end() cleans up by itself on failure
    if (!Update.end(true)) {
        DBG(NET, "ota: Update.end failed: %s", Update.errorString());
        otaCleanup();
        gOtaBusy = false;
        strlcpy(gOtaLastErr, "Image rejected", sizeof(gOtaLastErr));
        otaPostErr("Image rejected", r.e.ver);
        return;
    }
    otaCleanup();
    gOtaBusy = false;
    gOtaInstalled = true;
    DBG(NET, "ota: v%s installed, boot partition switched", r.e.ver);
    uiPost(UE_OTA, r.e.ver, "Installed", OTA_DONE, 100);
}

// Open the firmware stream starting at byte "from" (0 = whole file, otherwise HTTP Range resume).
// Tries several GitHub addresses.  On failure it calls otaFail() itself and returns false.
static bool otaOpenStream(uint32_t from) {
    OtaRun& r = gOtaR;
    int code = -1;
    for (int k = 0; k < OTA_CONNECT_TRIES; k++) {
        int att = (r.attempt + k) % OTA_CONNECT_TRIES;
        r.conn.close();
        if (WiFi.status() != WL_CONNECTED) {
            otaFail("Wi-Fi lost");
            return false;
        }
        if (!r.conn.open(r.e.url, att)) {
            otaFail("Out of memory");
            return false;
        }
        if (from) {
            char h[32];
            snprintf(h, sizeof(h), "bytes=%u-", (unsigned)from);
            r.conn.http->addHeader("Range", h);
        }
        code = r.conn.preFailed ? -1 : r.conn.http->GET();
        DBG(NET, "ota: download GET from=%u (address %d) -> %d (%s)", (unsigned)from, att, code, HTTPClient::errorToString(code).c_str());
        if (code != -1) {
            r.attempt = att;
            break;
        }
    }
    if (from && code == 200) {
        otaFail("Server cannot resume");
        return false;
    }
    if (code != (from ? 206 : 200)) {
        char m[40];
        otaHttpErr(code, m, sizeof(m));
        otaFail(m);
        return false;
    }
    if (r.conn.http->getSize() != (int)(r.total - from)) {
        otaFail("Size mismatch");
        return false;
    }
    r.lastData = millis();
    return true;
}

// The connection died or stalled: reconnect and continue where we stopped. The flash and the SHA-256 simply carry on.
static void otaResume(const char* why) {
    OtaRun& r = gOtaR;
    if (++r.resumes > OTA_MAX_RESUMES) return otaFail("Network too unstable");
    if (millis() - r.lastBytesAt > OTA_GIVEUP_MS) return otaFail("No data for 60 s - weak network");
    DBG(NET, "ota: %s at %u/%u bytes -> resume %d/%d (free heap %u, largest %u)", why, (unsigned)r.got, (unsigned)r.total, (int)r.resumes, OTA_MAX_RESUMES, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    char rt[24];
    snprintf(rt, sizeof(rt), "Reconnecting %d", (int)r.resumes);
    uiPost(UE_OTA, r.e.ver, rt, OTA_PROGRESS, r.lastPct < 0 ? 0 : r.lastPct);
    otaOpenStream(r.got);
}

// One slice (~25 ms) of the download.  Called from otaNetTick() every netTask loop.
static void otaDlStep() {
    OtaRun& r = gOtaR;
    uint32_t t0 = millis();
    if (WiFi.status() != WL_CONNECTED) return otaFail("Wi-Fi lost");
    if (t0 - r.startedAt > OTA_TOTAL_MS) return otaFail("Download timed out");
    WiFiClient* s = r.conn.http->getStreamPtr();
    uint8_t buf[1024];
    while (millis() - t0 < 25) {
        int av = s->available();
        if (av <= 0) {
            if (!s->connected()) return otaResume("Connection lost");
            if (millis() - r.lastData > OTA_STALL_MS) return otaResume("Download stalled");
            break;
        }
        size_t want = (size_t)av;
        if (want > sizeof(buf)) want = sizeof(buf);
        if (want > r.total - r.got) want = r.total - r.got;
        int n = s->readBytes(buf, want);
        if (n <= 0) break;
        if (Update.write(buf, (size_t)n) != (size_t)n) {
            DBG(NET, "ota: Update.write failed: %s", Update.errorString());
            return otaFail(Update.getError() == UPDATE_ERROR_MAGIC_BYTE ? "Not a firmware image" : "Flash write failed");
        }
        mbedtls_sha256_update(&r.sha, buf, (size_t)n);
        r.got += (uint32_t)n;
        r.lastData = r.lastBytesAt = millis();
        if (r.got >= r.total) return otaFinish();
    }
    int pct = (int)(((uint64_t)r.got * 100) / r.total);
    if (pct != r.lastPct) {
        r.lastPct = pct;
        uiPost(UE_OTA, r.e.ver, "Downloading", OTA_PROGRESS, pct);
    }
}

// NC_OTA_START (a = manifest index, s1 = version, which must still match: the list may have been re-read).
static void otaStartDownload(int idx, const char* ver) {
    if (gOtaBusy) return;
    if (idx < 0 || idx >= gOtaN || strcmp(gOtaMan[idx].ver, ver) != 0) return otaPostErr("Version list changed", ver);
    if (WiFi.status() != WL_CONNECTED) return otaPostErr("No Wi-Fi connection", ver);
    if (anyGameActive()) return otaPostErr("Online game in progress", ver);
    if (!otaPowerOk()) return otaPostErr("Battery too low", ver);

    OtaRun& r = gOtaR;
    r.e = gOtaMan[idx];
    r.total = r.e.size;
    r.got = 0;
    r.lastPct = -1;
    r.updBegun = false;
    r.shaInit = false;
    r.startedAt = r.lastData = r.lastBytesAt = millis();

    gOtaBusy = true;                                    // pause all polling in netTask
    hReq.close();                                       // give the ~45 KB of the kept-alive socket back
    uint32_t fh = ESP.getFreeHeap(), blk = ESP.getMaxAllocHeap();
    DBG(NET, "ota: starting v%s, free heap %u, largest block %u", r.e.ver, (unsigned)fh, (unsigned)blk);
    if (fh < OTA_MIN_HEAP || blk < OTA_MIN_BLOCK) {
        char m[40];
        snprintf(m, sizeof(m), "Low memory (%uK free)", (unsigned)(fh / 1024));
        return otaFail(m);
    }
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (!next || r.total > next->size) return otaFail("Image too big");
    // Update.begin() first: it needs a 4 KB buffer, and once the TLS session is open the largest free block is only ~3 KB.
    if (!Update.begin(r.total, U_FLASH)) {                // writes to the inactive slot
        const esp_partition_t* run = esp_ota_get_running_partition();
        DBG(NET, "ota: Update.begin failed: %s (code %u) bin=%u next=%s/%u running=%s/%u free heap %u largest %u", Update.errorString(), (unsigned)Update.getError(), (unsigned)r.total,
            next ? next->label : "none", next ? (unsigned)next->size : 0, run ? run->label : "none", run ? (unsigned)run->size : 0, (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        char fm[40];
        snprintf(fm, sizeof(fm), "Flash start: %s", Update.errorString());
        return otaFail(fm);
    }
    r.updBegun = true;                                     // from here every failure path aborts the update (otaFail)
    r.attempt = 0;
    r.resumes = 0;
    if (!otaOpenStream(0)) return;                         // otaFail() was already called
    mbedtls_sha256_init(&r.sha);
    mbedtls_sha256_starts(&r.sha, 0);
    r.shaInit = true;
    uiPost(UE_OTA, r.e.ver, "Downloading", OTA_PROGRESS, 0);
}

static void otaNetCmd(const NetCmd& c) {
    if (c.t == NC_OTA_CHECK) otaManualCheck();
    else if (c.t == NC_OTA_START) otaStartDownload(c.a, c.s1);
}

// Called by netTask right after wifiTick().  Returns true while a download runs: the caller then skips
// everything else (accounts, polling, streams, housekeeping) for this loop.
static bool otaIsBusy() { return gOtaBusy; }

// Called by netTask right after wifiTick() (see above).
static bool otaNetTick() {
    uint32_t now = millis();
    bool conn = WiFi.status() == WL_CONNECTED;
    if (conn && !gOtaWasConn) {                          // Wi-Fi connect edge
        gOtaWifiUpAt = now | 1;
        gOtaManifestDue = true;
        gOtaTries = 0;
        gOtaNextTry = now + 4000;                        // let the connection settle first
    } else if (!conn) {
        gOtaWifiUpAt = 0;
    }
    gOtaWasConn = conn;

    // Rollback: the new image is trusted only after Wi-Fi has been up and stable for OTA_VALID_AFTER_MS.
    if (gOtaNeedValid && conn && gOtaWifiUpAt && now - gOtaWifiUpAt >= OTA_VALID_AFTER_MS) {
        esp_err_t er = esp_ota_mark_app_valid_cancel_rollback();
        DBG(NET, "ota: new firmware marked valid (%d)", (int)er);
        if (er == ESP_OK) gOtaNeedValid = false;
    }

    if (gOtaBusy) {
        otaDlStep();
        return true;
    }

    if (conn && gOtaManifestDue && (int32_t)(now - gOtaNextTry) >= 0) {
        if (!anyGameActive() && ESP.getFreeHeap() < OTA_CHECK_MIN_HEAP) hReq.close();   // give back the kept-alive Lichess socket first
        if (anyGameActive() || ESP.getFreeHeap() < OTA_CHECK_MIN_HEAP) {
            DBG(NET, "ota: check deferred (game=%d, free heap %u, need %u)", (int)anyGameActive(), (unsigned)ESP.getFreeHeap(), (unsigned)OTA_CHECK_MIN_HEAP);
            gOtaNextTry = now + 15000;                   // not now: retry when idle
        } else {
            char err[40];
            if (otaFetchManifest(LATEST_URL, OTA_LATEST_MAX, err, sizeof(err))) {
                gOtaManifestDue = false;
                gOtaOfferDue = true;
            } else if (++gOtaTries >= OTA_MANIFEST_TRIES) {
                gOtaManifestDue = false;                 // give up until the next Wi-Fi connect
            } else {
                gOtaNextTry = millis() + 20000UL * gOtaTries;
            }
        }
    }
    if (gOtaOfferDue && !anyGameActive() && otaPowerOk()) {
        gOtaOfferDue = false;
        otaPostAuto();
    }
    return false;
}

// ============================================================================================
//                                       UI SIDE  (core 0)
// ============================================================================================
enum OtaAfter : uint8_t { OTA_AFTER_RESTART, OTA_AFTER_OFF };

static char gOtaVer[12] = "";            // newest offered version (from the last OFFER / INDEX event)
static char gOtaNote[32] = "";
static int gOtaIdx = -1;                 // its manifest index, -1 = none
static char gOtaPostponed[12] = "";      // user pressed Cancel on this version: not offered again until reboot
static bool gOtaOfferWaiting = false;    // an offer is waiting for an idle moment
static uint32_t gOtaLastTry = 0;

static bool gOtaRun = false;             // UI side of an update is in progress (progress page shown)
static uint8_t gOtaAfter = OTA_AFTER_RESTART;
static PageId gOtaReturn = PG_SETTINGS;
static int gOtaPct = 0;
static char gOtaTxt[32] = "";
static char gOtaTitleVer[12] = "";
static bool gOtaDonePending = false;
static uint32_t gOtaDoneAt = 0;

static bool gOtaAboutList = false;       // About page shows the version list
static int gOtaPickIdx = -1;
static char gOtaPickVer[12] = "";
static bool gOtaPowerRestart = false;

static void otaNopCb(int) {}

static void otaSetPend(bool b) {
    if (gOtaPend == b) return;
    gOtaPend = b;
    gPrefs.putBool("ota_pend", b);
}

// Called once from setup(), after storageBegin().
static void otaBegin() {
    if (gPrefs.isKey("ota_skip")) gPrefs.getString("ota_skip", gOtaSkip, sizeof(gOtaSkip));
    gOtaPend = gPrefs.getBool("ota_pend", false);
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK) {
        gOtaNeedValid = (st == ESP_OTA_IMG_PENDING_VERIFY);
        DBG(SYS, "ota: running slot %s, image state %d (%s)", run->label, (int)st, gOtaNeedValid ? "PENDING_VERIFY: will validate after Wi-Fi + 10 s" : "no rollback pending");
    } else {
        DBG(SYS, "ota: no rollback state available for the running slot");
    }
    DBG(SYS, "ota: skip='%s' pend=%d", gOtaSkip, (int)gOtaPend);
}

// Not in a game, setup, wait screen, modal, or an update already; screen on.
static bool otaUiIdle() {
    if (gModal != MD_NONE || gScreenOff || gOtaRun) return false;
    if (gCur == PG_GAME || gCur == PG_SETUP || gCur == PG_WAIT || gCur == PG_OTA) return false;
    if (gCur == PG_ABOUT && gOtaAboutList) return false;
    if (setupActive() || physLive() != nullptr) return false;
    return true;
}

// ---- progress page (registered as PG_OTA, no toolbar, no touch) ----
static void otaBarDraw() {
    if (gScreenOff || gCur != PG_OTA) return;
    tft.fillRect(10, 150, 220, 100, T.bg);
    tft.drawRoundRect(20, 170, 200, 16, 4, T.card2);
    int w = (196 * gOtaPct) / 100;
    if (w > 0) tft.fillRoundRect(22, 172, w, 12, 3, T.accent);
    char s[16];
    snprintf(s, sizeof(s), "%d%%", gOtaPct);
    gfxText(120, 198, s, 2, T.fg, TD_TC);
    gfxText(120, 228, gOtaTxt, 1, T.dim, TD_TC);
}

static void otaPageDraw() {
    tft.fillScreen(T.bg);
    gfxText(120, 70, "Updating Gchess", 2, T.fg, TD_TC);
    char s[24];
    snprintf(s, sizeof(s), "v%s", gOtaTitleVer);
    gfxText(120, 110, s, 2, T.accent, TD_TC);
    gfxText(120, 268, "Keep the board powered on", 1, T.dim, TD_TC);
    otaBarDraw();
}

#if OTA_SLIM_BOOT
// Start an update (UI side): remember the job in flash and restart. The download itself runs in otaSlimBoot()
// right after the restart, when almost all RAM is free (the running firmware has only ~9 KB left with a TLS socket open).
static void otaStart(int idx, const char* ver, uint8_t after) {
    if (gOtaRun) return;
    if (WiFi.status() != WL_CONNECTED) { uiToast("No Wi-Fi connection"); return; }
    if (!otaPowerOk()) { uiToast("Battery too low for update"); return; }
    if (idx < 0 || idx >= gOtaN || strcmp(gOtaMan[idx].ver, ver) != 0) { uiToast("Version list changed"); return; }
    gPrefs.putBytes("ota_ent", &gOtaMan[idx], sizeof(OtaEntry));
    gPrefs.putUChar("ota_after", after);
    gPrefs.putBool("ota_job", true);
    otaSetPend(false);
    DBG(SYS, "ota: job saved for v%s, restarting into the slim update boot", ver);
    tft.fillScreen(T.bg);                                     // clean screen before the restart, nothing of the old page is left behind
    gfxText(120, 150, "Restarting to update...", 1, T.fg, TD_TC);
    delay(300);
    powerRestart();
}
#else
// Start an update (UI side): show the progress page, then ask the net task.
static void otaStart(int idx, const char* ver, uint8_t after) {
    if (gOtaRun) return;
    if (WiFi.status() != WL_CONNECTED) { uiToast("No Wi-Fi connection"); return; }
    if (!otaPowerOk()) { uiToast("Battery too low for update"); return; }
    gOtaRun = true;
    gOtaAfter = after;
    gOtaDonePending = false;
    gOtaPct = 0;
    strlcpy(gOtaTxt, "Connecting...", sizeof(gOtaTxt));
    strlcpy(gOtaTitleVer, ver, sizeof(gOtaTitleVer));
    gOtaReturn = (gCur == PG_OTA) ? PG_SETTINGS : gCur;
    uiGo(PG_OTA);
    netCmd(NC_OTA_START, ver, "", idx);
}
#endif

// ---------------------------------------------------------------------------------------------
//  SLIM UPDATE BOOT: call from setup() after the queues exist and BEFORE gamesInit().
//  Returns false when no update job is stored (normal start). Otherwise it never returns.
// ---------------------------------------------------------------------------------------------
static void otaSlimDraw(int pct, const char* txt) {
    tft.fillRect(0, 130, 240, 190, T.bg);
    gfxText(120, 156, txt, 1, T.fg, TD_TC);
    tft.drawRect(20, 190, 200, 16, T.card2);
    int w = (196 * constrain(pct, 0, 100)) / 100;
    if (w > 0) tft.fillRect(22, 192, w, 12, T.accent);
    char s[8];
    snprintf(s, sizeof(s), "%d%%", pct);
    gfxText(120, 218, s, 2, T.accent, TD_TC);
    gfxText(120, 268, "Do not switch off", 1, T.dim, TD_TC);
}

static bool otaSlimWifi() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < MAX_WIFI; i++) {
            if (!gWifi[i].used) continue;
            char t[48];
            snprintf(t, sizeof(t), "Connecting to %.24s...", gWifi[i].ssid);
            otaSlimDraw(0, t);
            WiFi.begin(gWifi[i].ssid, gWifi[i].pass);
            for (uint32_t t0 = millis(); millis() - t0 < 12000;) {
                if (WiFi.status() == WL_CONNECTED) return true;
                delay(100);
            }
            WiFi.disconnect(false, false);
        }
    }
    return false;
}

static bool otaSlimBoot() {
    if (!gPrefs.getBool("ota_job", false)) return false;
    gPrefs.putBool("ota_job", false);                          // clear FIRST: whatever happens, we never loop in this mode
    OtaEntry e;
    if (gPrefs.getBytes("ota_ent", &e, sizeof(e)) != sizeof(e)) return false;
    uint8_t after = gPrefs.getUChar("ota_after", OTA_AFTER_RESTART);
    DBG(SYS, "ota: SLIM BOOT for v%s, free heap %u, largest %u", e.ver, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    tft.fillScreen(T.bg);                                     // wipe whatever the previous firmware / the display RAM still shows
    gfxText(120, 60, "Updating firmware", 2, T.accent, TD_TC);
    char vs[24];
    snprintf(vs, sizeof(vs), "to v%s", e.ver);
    gfxText(120, 92, vs, 2, T.fg, TD_TC);

    bool ok = false;
    const char* err = "";
    if (!otaSlimWifi()) {
        err = "Wi-Fi not available";
    } else {
        gOtaMan[0] = e;
        gOtaN = 1;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            gOtaInstalled = false;
            gOtaLastErr[0] = 0;
            otaSlimDraw(0, attempt ? "Trying again..." : "Connecting to GitHub...");
            otaStartDownload(0, e.ver);
            int last = -1;
            while (gOtaBusy) {
                otaDlStep();
                xQueueReset(qUi);                                // nobody reads the UI events in this mode
                int pct = gOtaR.total ? (int)(((uint64_t)gOtaR.got * 100) / gOtaR.total) : 0;
                if (pct != last) {
                    last = pct;
                    otaSlimDraw(pct, gOtaR.resumes ? "Downloading (reconnected)" : "Downloading");
                }
                delay(1);
            }
            ok = gOtaInstalled;
            if (!ok) {
                err = gOtaLastErr[0] ? gOtaLastErr : "Could not start";
                if (!strcmp(err, "Hash mismatch") || !strcmp(err, "Image rejected") || !strcmp(err, "Not a firmware image")) break;   // retrying will not help
                otaSlimDraw(0, err);
                delay(3000);
            }
        }
    }
    if (ok) {
        otaSlimDraw(100, "Installed. Restarting...");
        delay(1500);
        if (after == OTA_AFTER_OFF) {                           // same final steps as powerOff()
            while (powerReadButton()) delay(10);
            digitalWrite(PIN_POWER, LOW);
            delay(300);
        }
    } else {
        tft.fillRect(0, 130, 240, 190, T.bg);
        gfxText(120, 156, "Update failed", 2, T.bad, TD_TC);
        gfxText(120, 190, err, 1, T.fg, TD_TC);
        gfxText(120, 214, "Current version kept", 1, T.dim, TD_TC);
        delay(5000);
    }
    ESP.restart();
    while (true) delay(1000);
}

static void otaFailUi(const char* why) {
    gOtaRun = false;
    gOtaDonePending = false;
    if (gCur == PG_OTA) uiGo(gOtaReturn);
    char m[96];
    snprintf(m, sizeof(m), "%.60s\nCurrent version kept.", why);
    // No power action follows a failed update (also for "Update + off" / "Update + restart").
    uiDialog("Update failed", m, "OK", nullptr, otaNopCb);
}

// ---- About page: Update button + version list ----
static void otaAboutLeaveList() {
    if (!gOtaAboutList) return;
    gOtaAboutList = false;
    if (gCur == PG_ABOUT) {
        tft.fillScreen(T.bg);
        uiRedraw();
    }
}

static void otaAboutEnter() { gOtaAboutList = false; }

static void otaAboutDraw() {
    if (gOtaAboutList) {
        listDraw();
        return;
    }
    aboutDraw();
    gfxBtn(60, 272, 120, 32, "Update", T.card2, T.accent);
}

static void otaPickCb(int b) {
    if (b == OTA_B2) otaStart(gOtaPickIdx, gOtaPickVer, OTA_AFTER_RESTART);
}

static void otaPick(int i) {
    ListItem& it = gList[LK_OTA].it[i];
    strlcpy(gOtaPickVer, it.id, sizeof(gOtaPickVer));
    gOtaPickIdx = it.val;
    int c = otaVerCmp(gOtaPickVer, GC_VERSION);
    char m[96];
    snprintf(m, sizeof(m), "%s v%s?\nThe board restarts afterwards.", c == 0 ? "Reinstall" : (c < 0 ? "Downgrade to" : "Install"), gOtaPickVer);
    uiDialog("Install update", m, "Cancel", "Install", otaPickCb);
}

static void otaAboutTouch(TouchPhase ph, int x, int y) {
    if (gOtaAboutList) {
        int i = uiListTouch(lpv, ph, x, y, rowDraw);
        if (i >= 0) {
            uiTap();
            otaPick(i);
        }
        return;
    }
    if (ph == TP_UP && y >= 272 && y < 304 && x >= 60 && x < 180) {
        uiTap();
        gOtaAboutList = true;
        gList[LK_OTA].n = 0;
        listSetup(LK_OTA, 40);
        tft.fillScreen(T.bg);
        uiRedraw();                                      // draws "Loading..." through otaAboutDraw -> listDraw
        netCmd(NC_OTA_CHECK, "", "", 1);
    }
}

static void otaAboutEvent(const UiEvt& e) {
    if (gOtaAboutList && e.t == UE_LIST) listEvent(e);
}

// ---- offer dialog ----
static void otaOfferCb(int b) {
    if (b == OTA_B1) {
        otaStart(gOtaIdx, gOtaVer, OTA_AFTER_RESTART);
    } else if (b == OTA_B2) {                            // Skip: never offer exactly this version again
        strlcpy(gOtaSkip, gOtaVer, sizeof(gOtaSkip));
        gPrefs.putString("ota_skip", gOtaSkip);
        otaSetPend(false);
        gOtaIdx = -1;
        uiToast("Version skipped");
    } else if (b == OTA_B3) {                            // Cancel: ask again at the next power off / restart
        otaSetPend(true);
        strlcpy(gOtaPostponed, gOtaVer, sizeof(gOtaPostponed));
        uiToast("Update at next power off");
    }
}

static void otaOfferShow() {
    char m[96];
    if (gOtaNote[0]) snprintf(m, sizeof(m), "Version %s is ready.\n%.34s", gOtaVer, gOtaNote);
    else snprintf(m, sizeof(m), "Version %s is ready.\nSkip hides this version.", gOtaVer);
    uiDialog3("Update available", m, "Update", "Skip", "Cancel", otaOfferCb);
}

// ---- power-off / restart interception (ui.h PE_OFF / PE_RESTART handler) ----
static void otaPowerCb(int b) {
    if (b == OTA_B1) otaStart(gOtaIdx, gOtaVer, gOtaPowerRestart ? OTA_AFTER_RESTART : OTA_AFTER_OFF);
    else if (gOtaPowerRestart) powerRestart();
    else powerOff();
}

// Returns true if it opened the dialog (the caller must then NOT power off / restart now).
static bool otaPowerIntercept(bool restart) {
    if (!gOtaPend || gOtaIdx < 0 || gOtaRun) return false;
    if (WiFi.status() != WL_CONNECTED || !otaPowerOk()) return false;
    // Not otaUiIdle(): the Power box that called us is itself a dialog. Only refuse on screens where an update must not start.
    if (gCur == PG_GAME || gCur == PG_SETUP || gCur == PG_WAIT || gCur == PG_OTA || setupActive() || physLive() != nullptr) return false;
    gOtaPowerRestart = restart;
    char m[96];
    snprintf(m, sizeof(m), "Version %s is ready.\nInstall it before %s?", gOtaVer, restart ? "restart" : "power off");
    // Button text buffers hold 15 characters.
    uiDialog("Update pending", m, restart ? "Update+restart" : "Update + off", restart ? "Restart" : "Power off", otaPowerCb);
    return true;
}

// ---- UE_OTA events (called first thing in globalEvt) ----
static bool otaUiEvent(const UiEvt& e) {
    char t[48];
    switch (e.a) {
        case OTA_OFFER:
        case OTA_INDEX:
            strlcpy(gOtaVer, e.s1, sizeof(gOtaVer));
            strlcpy(gOtaNote, e.s2, sizeof(gOtaNote));
            gOtaIdx = e.b;
            if (e.a == OTA_OFFER && strcmp(gOtaVer, gOtaPostponed) != 0) gOtaOfferWaiting = true;
            break;
        case OTA_UPTODATE:
            gOtaIdx = -1;
            gOtaOfferWaiting = false;
            otaSetPend(false);
            if (e.b) uiToast("You have the latest version");
            break;
        case OTA_PROGRESS:
            if (!gOtaRun) break;
            gOtaPct = e.b;
            strlcpy(gOtaTxt, e.s2, sizeof(gOtaTxt));
            otaBarDraw();
            break;
        case OTA_DONE:
            if (!gOtaRun) break;
            gOtaPct = 100;
            strlcpy(gOtaTxt, "Installed. Restarting...", sizeof(gOtaTxt));
            otaBarDraw();
            otaSetPend(false);
            gOtaDonePending = true;
            gOtaDoneAt = millis() + 1500;
            break;
        case OTA_ERROR:
            otaFailUi(e.s2);
            break;
        case OTA_CHECK_FAIL:
            otaAboutLeaveList();
            snprintf(t, sizeof(t), "Update check: %.30s", e.s2);
            uiToast(t);
            break;
        default:
            break;
    }
    return true;
}

// Called every uiTask loop (~60 Hz): delayed power action after a successful update, and the "retry the offer when idle".
static void otaUiTick() {
    uint32_t now = millis();
    if (gOtaDonePending && (int32_t)(now - gOtaDoneAt) >= 0) {
        gOtaDonePending = false;
        if (gOtaAfter == OTA_AFTER_OFF) powerOff();      // existing paths: save storage + board map first
        else powerRestart();
        return;
    }
    if (gOtaOfferWaiting && now - gOtaLastTry > 3000) {
        gOtaLastTry = now;
        if (gOtaIdx >= 0 && otaUiIdle()) {
            gOtaOfferWaiting = false;
            otaOfferShow();
        }
    }
}