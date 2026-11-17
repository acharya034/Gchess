// leds.h - 64 square LEDs, snake wired, LED 0 = first square (no sacrificial LED).
// Square numbering: a1=0 ... h8=63.
// Physical board: R1C1 = h8, R8C8 = a1.  LED 0 sits at R1C8.
#pragma once
#include <FastLED.h>
#include "gchess_config.h"
#include "storage.h"

static CRGB leds[LED_COUNT];
static CRGB ledLayer[64];
static CRGB ledOverlay[64];
static uint32_t ledOverlayUntil[64];
static bool ledDirty = true;
static uint32_t ledLastShow = 0;

// (row, col) 0-based, row 0 / col 0 = R1C1 -> chess square
// default:  sq = (7-row)*8 + (7-col)   (R1C1 = h8, R8C8 = a1)
// flipHw:   board turned 180 degrees
inline int hwToSq(int row, int col) {
    return gSet.flipHw ? (row * 8 + col) : ((7 - row) * 8 + (col));
}

inline void sqToHw(int sq, int* row, int* col) {
    int f = sq & 7, r = sq >> 3;
    if (gSet.flipHw) { *row = r;     *col = f;     }
    else             { *row = 7 - r; *col = 7 - f; }
}

// Snake: 0-based row 0 runs C8->C1, row 1 runs C1->C8, ...
inline int sqToLed(int sq) {
    int row, col;
    sqToHw(sq, &row, &col);
    return row * 8 + (!(row & 1) ? col : 7 - col);
}

inline void ledSetBrightness(uint8_t b) {
    gSet.ledBright = b;
    FastLED.setBrightness(b);
    ledDirty = true;
}

inline void ledsBegin() {
    FastLED.addLeds<WS2812B, PIN_LED_DATA, GRB>(leds, LED_COUNT);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, LED_MAX_MA);
    FastLED.setBrightness(gSet.ledBright);
    memset(ledLayer, 0, sizeof(ledLayer));
    memset(ledOverlay, 0, sizeof(ledOverlay));
    memset(ledOverlayUntil, 0, sizeof(ledOverlayUntil));
    FastLED.clear(true);
}

inline void ledsClear() {
    memset(ledLayer, 0, sizeof(ledLayer));
    ledDirty = true;
}

// Stop all flashing overlays (ledsClear() only clears the base layer).
inline void ledsClearOverlay() {
    memset(ledOverlayUntil, 0, sizeof(ledOverlayUntil));
    ledDirty = true;
}

inline void ledSq(int sq, CRGB c) {
    if (sq >= 0 && sq < 64) { ledLayer[sq] = c; ledDirty = true; }
}

inline void ledsFlash(int sq, CRGB c, uint16_t ms) {
    if (sq >= 0 && sq < 64) {
        ledOverlay[sq] = c;
        ledOverlayUntil[sq] = millis() + ms;
        ledDirty = true;
    }
}

inline void ledsFlashMask(uint64_t m, CRGB c, uint16_t ms) {
    for (int i = 0; i < 64; i++) if (m >> i & 1) ledsFlash(i, c, ms);
}

inline void ledsShowLastMove(int from, int to) {
    if (from >= 0) ledSq(from, CRGB(120, 65, 0));   // Soft Amber (From)
    if (to >= 0)   ledSq(to,   CRGB(0, 180, 255));  // Bright Cyan (To)
}

inline void ledsShowOptions(int from, uint64_t dest, uint64_t caps) {
    ledsClear();
    ledSq(from, CRGB(0, 70, 255));
    for (int i = 0; i < 64; i++)
        if (dest >> i & 1) ledSq(i, (caps >> i & 1) ? CRGB(255, 20, 0) : CRGB(0, 255, 60));
}

inline void ledsRainbowTest() {
    for (int sq = 0; sq < 64; sq++) ledLayer[sq] = CHSV(sq * 4, 255, 255);
    ledDirty = true;
}

inline void ledsTick() {
    uint32_t now = millis();
    static uint8_t lastPhase = 0;
    bool anyFlash = false;
    for (int i = 0; i < 64; i++)
        if (ledOverlayUntil[i]) {
            if (now > ledOverlayUntil[i]) { ledOverlayUntil[i] = 0; ledDirty = true; }
            else anyFlash = true;
        }
    uint8_t phase = (now / 120) & 1;
    if (anyFlash && phase != lastPhase) ledDirty = true;   // make the blink animate
    lastPhase = phase;
    if (!ledDirty || now - ledLastShow < 16) return;
    ledDirty = false;
    ledLastShow = now;
    for (int sq = 0; sq < 64; sq++) {
        CRGB c = ledOverlayUntil[sq] ? ledOverlay[sq] : ledLayer[sq];
        if (ledOverlayUntil[sq] && phase) c = CRGB::Black;
        leds[sqToLed(sq)] = c;
    }
    FastLED.show();
}