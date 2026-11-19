// =====================================================================================
//  gchess_config.h  -  EVERYTHING you may want to change lives here
//  (pins, brand text, timings, debug switches)
// =====================================================================================
#pragma once
#include <Arduino.h>

// ---------- Brand -------------------------------------------------------------------
#define GC_NAME      "Gchess"
#define GC_TAGLINE   "Real pieces. Real games. Online."
#define GC_VERSION "1.0.0"
#define GC_HW      "A"
#define MANIFEST_URL "https://raw.githubusercontent.com/acharya034/Gchess/main/manifest.json"
#define LATEST_URL   "https://raw.githubusercontent.com/acharya034/Gchess/main/latest.json"

// ---------- Account linking website -------------------------------------------------
// The QR code on the board shows:  LINK_SITE_URL + "?d=<board ip>"
// The website then calls  http://<ip>/pair/start  and  http://<ip>/pair/token
#define LINK_SITE_URL "https://acharya034.github.io/Gchess-Server/"
#define LICHESS_HOST  "lichess.org"

// ---------- Serial debug switches (1 = print, 0 = silent) ---------------------------
// Open the serial monitor at 115200. Every line looks like:
//   [   12.345] C1 NET    >> GET https://lichess.org/api/account
//    time(s)    core cat   message
#define DBG_TOUCH   0   // every touch down / up / move (moves are rate limited)
#define DBG_UI      0  // page changes, buttons, menus, dialogs, notifications
#define DBG_EVT     0   // every message between core 0 (UI) and core 1 (network)
#define DBG_NET     1   // Wi-Fi, Lichess streams, game slots
#define DBG_HTTP    0   // full requests and responses (url, headers, body)
#define DBG_GAME    0  // moves, hints, wrong-move alarms, board ownership
#define DBG_SENS    0  // hall sensor changes (which squares changed)
#define DBG_POWER   0   // power button, battery, charger
#define DBG_SYS     1  // boot info, free memory every 10 s, task stack usage
#define DBG_SETUP   0   // board setup engine (put with the other DBG_ flags)
#define DEBUG_SHOW_SECRETS 0   // 1 = print full Lichess tokens (careful!)

// ---------- Pins (ESP32 DevKit, 30 pin) ---------------------------------------------
// TFT + touch share the SPI bus (VSPI)
#define PIN_TFT_SCLK 18
#define PIN_TFT_MOSI 23
#define PIN_TFT_MISO 19
#define PIN_TFT_CS   15
#define PIN_TFT_DC   17
#define PIN_TFT_RST  -1   // not connected: tie the display RESET pin to 3V3
#define PIN_TFT_BL    5   // backlight (PWM)
#define PIN_TOUCH_CS 21

#define PIN_LED_DATA  16   // addressable LEDs (through 330R into LED #0). Keep GPIO0 free of pull-downs.
#define PIN_BUZZER   3   // passive buzzer

// Power: ONE pin does everything (see wiring diagram "Power").
//   HIGH            = ON   (the ESP holds the latch; the capacitor is kept charged)
//   LOW for a moment = the pin is released (INPUT) for ~25 us to read the button; the capacitor
//                      keeps the latch closed for about 1 second, so this is invisible
//   LOW / INPUT held longer than the capacitor time (PWR_OFF_HOLD_MS) = OFF -> the latch releases
#define PIN_POWER     2

// Battery and charger readings (both must be ADC1 pins: 32..39)
#define PIN_BAT     32  // battery gauge: pulse it HIGH to charge the capacitor, then read it as ADC
#define PIN_CHG     33 // charger LEDs through two different resistors -> voltage tells the state


const int hallpin[3][4] = {
    {13, 12, 14, 27},   // positive lines
    {26, 25, 4, 22},   // negative lines
    {34, 35, 36, 39}    // data lines: quadrant 0..3 (same order as the working code)
};

// k = quadrant (0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right)
// i = positive line -> COLUMN inside the quadrant
// j = negative line -> ROW inside the quadrant
#define SENSOR_ROW(k, i, j)  (((k) >> 1) * 4 + (j))
#define SENSOR_COL(k, i, j)  (((k) & 1) * 4 + (i))

#define HALL_ACTIVE_LOW   1
#define HALL_SETTLE_US 1000    // the working code uses 1000; lower it later once it's stable
#define SENSOR_DEBOUNCE   4
#define SENSOR_SCAN_MS    2

#define PIN_LED_DATA     16
#define LED_COUNT        64    // no sacrificial LED any more
#define LED_MAX_MA     1500

// ---------- Power button timing -------------------------------------------------------
#define PWR_SAMPLE_MS    50    // how often the button is sampled
#define PWR_READ_US      25    // pull-up window length
#define BTN_RESTART_MS 2000    // hold this long  -> restart
#define BTN_OFF_MS     4000    // hold this long  -> power off
#define PWR_OFF_HOLD_MS 2000   // keep the pin LOW at least this long on power off (must be longer than the ~1 s the capacitor holds the latch)

// ---------- Battery gauge -------------------------------------------------------------
#define BAT_PERIOD_MS 20000    // measure every 20 s
#define BAT_PULSE_MS     15    // HIGH pulse that charges the capacitor
#define BAT_SETTLE_MS    40    // wait before reading
#define BAT_DIV         2.0f   // divider ratio (100k + 100k)
#define BAT_CAL         1.00f  // fine calibration: multiply result (measure with a multimeter)

// ---------- Charger LED reader (millivolts seen on PIN_CHG, 100k to GND at the pin) ---
// charging: ~2000 mV    full: ~900 mV    no USB: ~0 mV
// The values are printed on the serial monitor every second - adjust if yours differ.
#define CHG_MV_CHARGING_MIN 1500
#define CHG_MV_FULL_MIN      500

// ---------- Lichess connection / memory ------------------------------------------------
// One secure (TLS) connection costs ~45 KB of RAM and a new one needs ~70 KB free while it connects.
// An ESP32 without extra RAM cannot hold several at once, so:
//   * only MAX_LIVE_GAMES games get a permanent connection (a "stream"); every other game is
//     only a line in the Ongoing list (refreshed by polling)
//   * there is NO permanent "events" connection: challenges and new games are found by polling
//   * when a request needs memory, the live game stream is paused for a moment and resumes by itself
// GAME_LIVE_MODE = how the board follows the game you are playing
//   0 = POLLING (default). Moves AND game updates use ONE kept-alive TLS connection (~45 KB RAM).
//       A move needs a single round trip (well under 1 s). The opponent's move shows up within ~1 s.
//       No second connection -> no "SSL - Memory allocation failed". Clocks and draw offers are not shown.
//   1 = STREAM. A permanent game stream (instant moves, clocks, draw offers) PLUS the request connection =
//       two TLS sessions (~90 KB RAM). Only works if that much RAM is free (ESP32 with PSRAM).
#define GAME_LIVE_MODE 0
#define MAX_LIVE_GAMES (GAME_LIVE_MODE ? 1 : 2)   // polling can follow two games with the same single socket
#define POLL_GAME_OPP_MS      1000   // polling mode: read the game this often while the OPPONENT must move
#define POLL_GAME_MY_MS       3000   // ... and this often while it is MY move (catches resign / time-out)
#define HTTP_KEEPALIVE_IDLE_MS 20000 // close the kept-alive socket after this long without a request (and no game)
#define KEEPALIVE_MIN_FREE    30000  // do not keep the socket open if less RAM than this would be left
#define TLS_MIN_FREE_HEAP    100000  // stream mode: below this many free bytes the game stream is paused
#define POLL_CHALLENGES_MS 12000   // look for incoming challenges (when no game stream is open)
#define POLL_NEWGAME_MS     2500   // while waiting for a game to start
#define POLL_LIST_MS      120000   // refresh the list of running games

// ---------- Wi-Fi ----------------------------------------------------------------------
#define WIFI_WEAK_RSSI  (-78)  // below this we look for a better access point
#define WIFI_RESCAN_MS  20000
#define WIFI_RETRY_MS   12000

// ---------- Board setup engine ----------
#define SETUP_MAX_STEPS        32      // longest step list
#define SETUP_MAX_RESTORE_DIFF 16       // sensors differ from saved board by more than this squares -> UNKNOWN
#define LASTBRD_MIN_WRITE_MS   5000    // flash write rate limit for the "last board" memory
