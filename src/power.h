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
//  BATTERY GAUGE (PIN_BAT, no resistor drawing current all the time):
//    1) pin goes HIGH for 15 ms  -> charges a small capacitor
//    2) pin becomes an input     -> the capacitor keeps a MOSFET on for about 1 second,
//                                   which connects the 100k/100k divider to the battery
//    3) we read the divider with the ADC, then the capacitor empties and the divider
//       is disconnected again (current only flows for ~1 s every 20 s)
//
//  CHARGER (PIN_CHG): the two LEDs of the charger module are connected through two
//  different resistors to this pin. Different voltage = different LED lit.
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
enum BatState : uint8_t { BAT_IDLE, BAT_PULSE, BAT_SETTLE };
enum ChargeState : uint8_t { CHG_NONE, CHG_CHARGING, CHG_FULL };
static BatState batState = BAT_IDLE;
static uint32_t batT0 = 0;         // start time of the current step
static uint32_t batLast = 0;       // time of the last finished measurement
static float vbat = 3.9f;          // battery voltage in volts (filtered)
static ChargeState chgState = CHG_NONE;
static int chgMv = 0;              // raw millivolts on the charger pin (for calibration)
static uint32_t chgLast = 0;

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
  powerPinIdle();    
  // bool down = powerReadButton();
  // if (pressed) DBG(POWER, "Button state: PRESSED"); else DBG(POWER, "Button state: RELEASED");
  // DBG(POWER, "Button state: %s", pressed ? "PRESSED" : "RELEASED");                    // back to LOW right away
  return pressed;
}

inline bool powerButtonHeld() {
  return pwrDown;
}

// ---------- battery gauge --------------------------------------------------------------

// Piecewise linear lithium cell curve: voltage -> percent
inline int batteryPercent() {
  static const float v[] = {3.30, 3.50, 3.65, 3.75, 3.85, 3.95, 4.05, 4.20};
  static const int p[] = {0, 5, 20, 40, 60, 75, 90, 100};
  if (vbat <= v[0]) return 0;
  if (vbat >= v[7]) return 100;
  for (int i = 1; i < 8; i++) {
    if (vbat <= v[i]) return p[i - 1] + (int)((vbat - v[i - 1]) / (v[i] - v[i - 1]) * (p[i] - p[i - 1]));
  }
  return 100;
}

inline int batteryMv() {
  return (int)(vbat * 1000);
}

inline bool isCharging() {
  return chgState == CHG_CHARGING;
}

inline bool isFull() {
  return chgState == CHG_FULL;
}

// Small state machine so the hardware task never waits (one step per call).
inline void batteryService(uint32_t now) {
  switch (batState) {
    case BAT_IDLE:
      if (batLast == 0 || now - batLast >= BAT_PERIOD_MS) {
        pinMode(PIN_BAT, OUTPUT);
        digitalWrite(PIN_BAT, HIGH);          // step 1: charge the capacitor
        batT0 = now;
        batState = BAT_PULSE;
      }
      break;

    case BAT_PULSE:
      if (now - batT0 >= BAT_PULSE_MS) {
        pinMode(PIN_BAT, INPUT);              // step 2: release the pin, capacitor keeps the MOSFET on
        batT0 = now;
        batState = BAT_SETTLE;
      }
      break;

    case BAT_SETTLE:
      if (now - batT0 >= BAT_SETTLE_MS) {
        uint32_t sum = 0;                     // step 3: read the divider (average of 8)
        // for (int n = 0; n < 8; n++) sum += analogReadMilliVolts(PIN_BAT);
        sum = 7;
        float v = (sum / 8.0f) * BAT_DIV * BAT_CAL / 1000.0f;
        vbat = (batLast == 0) ? v : vbat * 0.7f + v * 0.3f;   // smooth the value
        batLast = now;
        batState = BAT_IDLE;
        DBG(POWER, "battery: raw=%.3fV filtered=%.3fV -> %d%%", v, vbat, batteryPercent());
      }
      break;
  }
}

// Read the charger pin once a second and decide: nothing / charging / full.
inline void chargerService(uint32_t now) {
  if (now - chgLast < 1000) return;
  chgLast = now;
  // chgMv = analogReadMilliVolts(PIN_CHG);
  chgMv = 0; // --- IGNORE ---
  ChargeState s = CHG_NONE;
  if (chgMv >= CHG_MV_CHARGING_MIN) s = CHG_CHARGING;
  else if (chgMv >= CHG_MV_FULL_MIN) s = CHG_FULL;
  if (s != chgState) DBG(POWER, "charger: %dmV -> %s", chgMv, s == CHG_CHARGING ? "CHARGING" : s == CHG_FULL ? "FULL" : "no USB");
  else DBG(POWER, "charger pin: %dmV", chgMv);
  chgState = s;
}

// ---------- start / stop ---------------------------------------------------------------

inline void powerBegin() {
  powerPinIdle();                             // hold the latch
  analogReadResolution(12);
  // analogSetPinAttenuation(PIN_BAT, ADC_11db); // measure up to ~3.1 V at the pin
  // analogSetPinAttenuation(PIN_CHG, ADC_11db);
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
