// =====================================================================================
//  power.h  -  power latch (ONE pin), push button, battery gauge, charger state
//
//  ONE PIN (PIN_POWER) is connected to: the push button and the latch circuit (with a capacitor).
//    * Board is off  : press the button -> the latch switches on and STAYS on after you
//                      release the button (no ESP needed).
//    * Board is on   : the ESP drives the pin HIGH (HIGH = ON). Every PWR_SAMPLE_MS it lets the
//                      pin go (INPUT) for PWR_READ_US microseconds to see if the button is pressed.
//                      The capacitor keeps the latch closed for about 1 s, so the board stays on.
//    * Power off     : the ESP drives the pin LOW (or leaves it as INPUT) for longer than the
//                      capacitor time (PWR_OFF_HOLD_MS) -> the latch releases.
//
//  BATTERY GAUGE (PIN_BAT): the divider is connected DIRECTLY to the battery (no MOSFET).
//    The pin stays an input and is read with the ADC every BAT_READ_MS.
//    (The divider always draws about 4.2 V / 147k = 29 uA.)
//
//  DIVIDER: 47k from battery (+) to the pin, 100k from the pin to GND.
//     Vpin = Vbat * 100 / (47 + 100)  ->  BAT_DIV must be (147.0f / 100.0f) = 1.47f
//     4.20 V battery -> 2.857 V at the pin (inside the 11 dB range of the ADC).
//     NOTE: ~32k source impedance is high for the ESP32 ADC. Put 100 nF from the pin to GND
//     (next to the pin) and keep the many-samples averaging below, otherwise the reading is
//     noisy / low and the percent jumps in big steps.
//
//  CHARGER (PIN_CHG): HIGH = charging. The charger LED BLINKS (1 s on / 1 s off) while charging,
//  so a LOW reading alone does NOT mean "not charging". We only say "not charging" when the pin
//  stayed LOW for longer than CHG_LOSS_MS (longer than one full blink period).
//
//  Public API:
//     powerBegin()         first line in setup()
//     powerTick()          call every ~1 ms from the hardware task
//     powerOff()           save everything and switch the board off
//     powerRestart()       save everything and reboot
//     batteryPercent(), batteryMv(), isCharging(), isFull(), powerButtonHeld()
// =====================================================================================
#pragma once
#include "gchess_config.h"
#include "debug.h"
#include "sound.h"
#include "storage.h"
#include "setup.h"
#include "analytics.h"
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <WiFi.h>
#include <math.h>

// ---- tuning (can be overridden in gchess_config.h) ----
#ifndef BAT_SAMPLES
#define BAT_SAMPLES 24          // ADC readings per measurement (min and max are thrown away)
#endif
#ifndef BAT_READ_MS
#define BAT_READ_MS 1000        // read the battery once a second (no switching circuit any more)
#endif
#ifndef BAT_FILTER_NEW
#define BAT_FILTER_NEW 0.35f    // weight of the new measurement (0.35 = fairly quick, still smooth)
#endif
#ifndef CHG_SAMPLE_MS
#define CHG_SAMPLE_MS 100       // read the charger pin 10 times a second (catches the blink)
#endif
#ifndef CHG_LOSS_MS
#define CHG_LOSS_MS 2500        // LOW for longer than this (blink is 1 s off) => not charging
#endif

// Events produced by the button; the UI task reads and clears gPwrEvt.
enum PwrEvt : uint8_t { PE_NONE, PE_SHORT, PE_RESTART, PE_OFF, PE_HOLD2, PE_HOLD4 };
static volatile PwrEvt gPwrEvt = PE_NONE;

// --- button state ---
static bool pwrArmed = false;      // becomes true after the start-up press was released
static bool pwrDown = false;       // button currently pressed
static bool pwr2 = false;          // 2 s mark already announced
static bool pwr4 = false;          // 4 s mark already announced
static uint32_t pwrDownAt = 0;     // when the press started
static uint32_t pwrLastSample = 0; // when the pin was sampled last

// --- battery / charger state ---
enum ChargeState : uint8_t { CHG_NONE, CHG_CHARGING, CHG_FULL };
static uint32_t batLast = 0;       // time of the last finished measurement
static float vbat = 3.9f;          // battery voltage in volts (filtered)
static ChargeState chgState = CHG_NONE;
static int chgMv = 0;              // raw millivolts on the charger pin (for calibration)
static uint32_t chgLast = 0;
static int chgLastHighMv = 0;      // last reading that was "lit"
static uint32_t chgLastHighAt = 0; // when that happened
static bool chgSeen = false;       // true once a "lit" reading was seen

// Optional hook: UI draws a goodbye screen before the board switches off.
static void (*gBeforeOff)() = nullptr;

// Set by powerOff(). From then on NOTHING else may touch PIN_POWER: the hardware task samples the
// button every PWR_SAMPLE_MS and each sample ends with powerPinIdle(), which would overwrite the
// "off" level and keep the latch closed.
static volatile bool gPowerOffing = false;

// ---------- button ---------------------------------------------------------------------

// Put the pin back to "idle HIGH output" (HIGH = ON: this is what holds the latch closed).
inline void powerPinIdle() {
  digitalWrite(PIN_POWER, HIGH);
  pinMode(PIN_POWER, OUTPUT);
}

// Look at the button for a very short moment. Returns true while it is pressed.
inline bool powerReadButton() {
  if (gPowerOffing) return false;          // power-off in progress: leave the pin alone
  pinMode(PIN_POWER, INPUT_PULLDOWN);     // let the pin float up (button open = HIGH)
  delayMicroseconds(PWR_READ_US);        // wait until the wire has reached its level
  bool pressed = (digitalRead(PIN_POWER) == HIGH);
  powerPinIdle();                          // back to HIGH right away
  return pressed;
}

inline bool powerButtonHeld() {
  return pwrDown;
}

// ---------- battery gauge --------------------------------------------------------------

// Piecewise linear lithium cell curve: voltage -> percent. Many points = smooth 1 % steps.
// (voltages must be ascending)
inline int batteryPercent() {
  static const float v[] = {3.27f, 3.61f, 3.69f, 3.71f, 3.73f, 3.75f, 3.77f, 3.79f, 3.80f, 3.82f, 3.84f,
                            3.85f, 3.87f, 3.91f, 3.95f, 3.98f, 4.02f, 4.08f, 4.11f, 4.15f, 4.20f};
  static const float p[] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50,
                            55, 60, 65, 70, 75, 80, 85, 90, 95, 100};
  const int N = sizeof(v) / sizeof(v[0]);
  if (vbat <= v[0]) return 0;
  if (vbat >= v[N - 1]) return 100;
  for (int i = 1; i < N; i++) {
    if (vbat <= v[i]) {
      float pc = p[i - 1] + (vbat - v[i - 1]) / (v[i] - v[i - 1]) * (p[i] - p[i - 1]);
      return (int)roundf(pc);              // 41, 42, 43 ... not only 40, 50, 60
    }
  }
  return 100;
}

inline int batteryMv() {
  return (int)(vbat * 1000.0f);
}

inline bool isCharging() {
  return chgState == CHG_CHARGING;
}

inline bool isFull() {
  return chgState == CHG_FULL;
}

// Average of many ADC readings; the lowest and highest are discarded (removes spikes).
inline uint32_t batteryReadPinMv() {
  uint32_t sum = 0, mn = 0xFFFFFFFF, mx = 0;
  for (int n = 0; n < BAT_SAMPLES; n++) {
    uint32_t r = analogReadMilliVolts(PIN_BAT);
    sum += r;
    if (r < mn) mn = r;
    if (r > mx) mx = r;
    delayMicroseconds(50);
  }
  sum -= mn + mx;
  return sum / (BAT_SAMPLES - 2);
}

// Divider is connected directly to the battery (no MOSFET), so just read it every BAT_READ_MS.
inline void batteryService(uint32_t now) {
  if (batLast != 0 && now - batLast < BAT_READ_MS) return;
  float pinMv = (float)batteryReadPinMv();
  float v = pinMv * BAT_DIV * BAT_CAL / 1000.0f;   // BAT_DIV = 1.47 for 47k + 100k
  // first value is taken as is; later values are smoothed
  vbat = (batLast == 0) ? v : vbat * (1.0f - BAT_FILTER_NEW) + v * BAT_FILTER_NEW;
  batLast = now ? now : 1;                          // never 0 (0 means "no reading yet")
  DBG(POWER, "battery: pin=%.0fmV raw=%.3fV filtered=%.3fV -> %d%%", pinMv, v, vbat, batteryPercent());
}

// Read the charger pin often and decide: nothing / charging / full.
// The charger LED blinks (1 s on, 1 s off) while charging, so:
//   * a "lit" reading  => charging right now
//   * LOW              => do NOT say "not charging" yet; keep the last state until the pin has
//                         stayed LOW for CHG_LOSS_MS (longer than a whole blink period)
inline void chargerService(uint32_t now) {
  if (now - chgLast < CHG_SAMPLE_MS) return;
  chgLast = now;
  chgMv = analogReadMilliVolts(PIN_CHG);

  if (chgMv >= CHG_MV_FULL_MIN) {             // pin is HIGH (some LED lit)
    chgLastHighMv = chgMv;
    chgLastHighAt = now;
    chgSeen = true;
  }

  ChargeState s = CHG_NONE;
  if (chgSeen && (now - chgLastHighAt) < CHG_LOSS_MS) {
    // use the last lit voltage, so the dark half of the blink does not change the answer
    s = (chgLastHighMv >= CHG_MV_CHARGING_MIN) ? CHG_CHARGING : CHG_FULL;
  } else {
    chgSeen = false;                          // LOW long enough: really not charging
  }

  if (s != chgState) {
    DBG(POWER, "charger: %dmV -> %s", chgMv, s == CHG_CHARGING ? "CHARGING" : s == CHG_FULL ? "FULL" : "no USB");
  }
  chgState = s;
}

// ---------- start / stop ---------------------------------------------------------------

inline void powerBegin() {
  powerPinIdle();                             // hold the latch
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT, ADC_11db); // measure up to ~3.1 V at the pin
  analogSetPinAttenuation(PIN_CHG, ADC_11db);
  pinMode(PIN_CHG, INPUT);
  pinMode(PIN_BAT, INPUT);
}

// Switch the board off. Everything is saved first.
[[noreturn]] inline void powerOff() {
  DBG(POWER, "powering off");
  if (gBeforeOff) gBeforeOff();               // goodbye screen
  soundPlay(SND_POWER_OFF);
  analyticsEvent("power_off");
  analyticsFlush();
  if (setupActive() && lastBoardValid() && boardOccupancy() != gLb.occ) lastBoardInvalidate();   // setup was half done: the saved map is no longer true
  lastBoardFlush();                           // saved board survives the power cut
  storageSaveAll();                           // data is safe now
  WiFi.disconnect(true, false);

  uint32_t t = millis();
  while (millis() - t < 900) {                // let the goodbye melody play
    soundTick();
    delay(5);
  }
  // Wait until the button is released (a pressed button holds the latch on). Give up after 3 s so a
  // stuck reading can never block the shutdown forever.
  t = millis();
  while (powerReadButton() && millis() - t < 3000) delay(10);

  // From here on the hardware task must not touch the pin any more (see gPowerOffing).
  gPowerOffing = true;
  delay(PWR_SAMPLE_MS + 10);                  // let a sample that was already running finish

  pinMode(PIN_POWER, OUTPUT);
  digitalWrite(PIN_POWER, LOW);               // the "off" signal
  delay(PWR_OFF_HOLD_MS);                     // longer than the ~1 s the capacitor holds the latch: on battery we die here

  // Still alive => powered by USB (the latch cannot cut USB power). Sleep until the button is pressed.
  // NOTE: wake level 1 matches powerReadButton() (pressed = HIGH). If your latch circuit pulls the pin
  // LOW when the button is pressed, change the level below to 0 and the pull to pull-up.
  rtc_gpio_pullup_dis((gpio_num_t)PIN_POWER);
  rtc_gpio_pulldown_en((gpio_num_t)PIN_POWER);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_POWER, 1);
  esp_deep_sleep_start();
  while (true) delay(1000);                   // never reached (keeps [[noreturn]] honest)
}

[[noreturn]] inline void powerRestart() {
  DBG(POWER, "restarting");
  analyticsFlush();
  if (setupActive() && lastBoardValid() && boardOccupancy() != gLb.occ) lastBoardInvalidate();   // setup was half done: the saved map is no longer true
  lastBoardFlush();                           // saved board survives the power cut
  storageSaveAll();
  delay(100);
  ESP.restart();
  while (true) delay(1000);
}

// ---------- main loop part (called every ~1 ms) ----------------------------------------

inline void powerTick() {
  if (gPowerOffing) return;                   // shutting down: do not touch the pin
  uint32_t now = millis();
  batteryService(now);
  chargerService(now);

  // Sample the button only every PWR_SAMPLE_MS (it costs a few microseconds each time).
  if (now - pwrLastSample < PWR_SAMPLE_MS) return;
  pwrLastSample = now;
  bool down = powerReadButton();

  // Ignore the press that switched the board on: wait until it was released once.
  if (!pwrArmed) {
    if (!down) pwrArmed = true;
    return;
  }

  if (down && !pwrDown) {                     // press started
    pwrDown = true;
    pwrDownAt = now;
    pwr2 = pwr4 = false;
    DBG(POWER, "button DOWN");
  } else if (down && pwrDown) {               // still pressed: announce 2 s and 4 s marks
    uint32_t held = now - pwrDownAt;
    if (!pwr2 && held >= BTN_RESTART_MS) {
      pwr2 = true;
      soundPlay(SND_WARN_HOLD);
      gPwrEvt = PE_HOLD2;
    }
    if (!pwr4 && held >= BTN_OFF_MS) {
      pwr4 = true;
      soundPlay(SND_WARN_HOLD);
      gPwrEvt = PE_HOLD4;
    }
  } else if (!down && pwrDown) {              // released: decide what the press meant
    uint32_t held = now - pwrDownAt;
    pwrDown = false;
    DBG(POWER, "button UP after %lu ms", (unsigned long)held);
    if (held >= BTN_OFF_MS) gPwrEvt = PE_OFF;
    else if (held >= BTN_RESTART_MS) gPwrEvt = PE_RESTART;
    else if (held > 40) gPwrEvt = PE_SHORT;
  }
}