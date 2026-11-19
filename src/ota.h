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
#ifndef OTA_MANIFEST_MAX
#define OTA_MANIFEST_MAX 8192       // bytes of manifest we are willing to read
#endif
#ifndef OTA_TIMEOUT_MS
#define OTA_TIMEOUT_MS 15000        // connect / handshake / response-header timeout
#endif
#ifndef OTA_STALL_MS
#define OTA_STALL_MS 20000          // no download bytes for this long -> abort
#endif
#ifndef OTA_TOTAL_MS
#define OTA_TOTAL_MS 600000UL       // whole download must finish within 10 minutes
#endif
#ifndef OTA_MAX_REDIRECTS
#define OTA_MAX_REDIRECTS 4
#endif
#ifndef OTA_MIN_HEAP
#define OTA_MIN_HEAP 70000          // free heap needed to start a download (TLS ~45 KB + Update buffer + HTTP objects)
#endif
#ifndef OTA_MIN_BLOCK
#define OTA_MIN_BLOCK 45000         // largest free block needed (TLS buffers are big contiguous allocations)
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
    bool open(const char* url) {
        cl = new (std::nothrow) WiFiClientSecure();
        http = new (std::nothrow) HTTPClient();
        if (!cl || !http) {
            close();
            return false;
        }
        otaTlsSetup(*cl);
        cl->setTimeout(OTA_TIMEOUT_MS / 1000);
        http->setConnectTimeout(OTA_TIMEOUT_MS);
        http->setTimeout(OTA_TIMEOUT_MS);
        http->setReuse(false);
        http->setUserAgent("Gchess/" GC_VERSION);
        http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // raw.githubusercontent.com / release assets
        http->setRedirectLimit(OTA_MAX_REDIRECTS);                 // redirect loop -> non-200 -> error
        if (!http->begin(*cl, url)) {
            close();
            return false;
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
static bool otaFetchManifest(const char* url, char* err, size_t en) {
    OtaConn c;
    if (!c.open(url)) {
        strlcpy(err, "Out of memory", en);
        return false;
    }
    bool ok = false;
    int code = c.http->GET();
    if (code != 200) {
        otaHttpErr(code, err, en);
    } else if (c.http->getSize() > (int)OTA_MANIFEST_MAX) {
        strlcpy(err, "Manifest too big", en);
    } else {
        char* buf = (char*)malloc(OTA_MANIFEST_MAX + 1);
        if (!buf) {
            strlcpy(err, "Out of memory", en);
        } else {
            OtaSink sink(buf, OTA_MANIFEST_MAX);
            int w = c.http->writeToStream(&sink);
            if (w < 0 || sink.over) {
                strlcpy(err, "Manifest read failed", en);
            } else {
                buf[sink.n] = 0;
                ok = otaParseManifest(buf);
                if (!ok) strlcpy(err, "Bad manifest", en);
            }
            free(buf);
        }
    }
    c.close();
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
    if (ESP.getFreeHeap() < TLS_MIN_FREE_HEAP) hReq.close();
    if (ESP.getFreeHeap() < TLS_MIN_FREE_HEAP) return otaPostCheckFail("Low memory");
    if (!otaFetchManifest(MANIFEST_URL, err, sizeof(err))) return otaPostCheckFail(err);
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
    uint32_t total = 0, got = 0, lastData = 0, startedAt = 0;
    int lastPct = -1;
    bool updBegun = false, shaInit = false;
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
        otaPostErr("Image rejected", r.e.ver);
        return;
    }
    otaCleanup();
    gOtaBusy = false;
    DBG(NET, "ota: v%s installed, boot partition switched", r.e.ver);
    uiPost(UE_OTA, r.e.ver, "Installed", OTA_DONE, 100);
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
            if (!s->connected()) return otaFail("Connection lost");
            if (millis() - r.lastData > OTA_STALL_MS) return otaFail("Download stalled");
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
        r.lastData = millis();
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
    r.startedAt = r.lastData = millis();

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
    if (!r.conn.open(r.e.url)) return otaFail("Out of memory");
    int code = r.conn.http->GET();
    if (code != 200) {
        char m[40];
        otaHttpErr(code, m, sizeof(m));
        return otaFail(m);
    }
    if (r.conn.http->getSize() != (int)r.total) return otaFail("Size mismatch");
    if (!Update.begin(r.total, U_FLASH)) {                // writes to the inactive slot
        DBG(NET, "ota: Update.begin failed: %s", Update.errorString());
        return otaFail("Cannot start flash write");
    }
    r.updBegun = true;
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
        if (anyGameActive() || ESP.getFreeHeap() < TLS_MIN_FREE_HEAP) {
            gOtaNextTry = now + 15000;                   // not now: retry when idle
        } else {
            char err[40];
            if (otaFetchManifest(LATEST_URL, err, sizeof(err))) {
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