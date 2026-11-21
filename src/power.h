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
//  CHARGER (NO PIN NEEDED any more): charging is found from the battery voltage and the power pin.
//    1) AT BOOT (powerBegin): PIN_POWER is read BEFORE we drive it.
//         HIGH = the user pressed the button            -> not charging
//         LOW  = the ESP got power from USB, no button  -> charging
//       Special cases: deep-sleep wake = USB is present (powerOff only sleeps on USB) -> charging.
//                      software restart / crash = pin test is not trustworthy (the capacitor
//                      holds the latch while the pin floats), so the state saved in RTC memory is used.
//    2) WHILE RUNNING (chargerDetect): a charger that is plugged in makes the battery voltage
//       jump up by a few 100 mV (3.96 -> 4.10). A slow baseline follows the voltage. If the raw voltage
//       stays CHG_JUMP_V above the baseline for CHG_CONFIRM readings -> charging.
//       If it stays CHG_DROP_V below the baseline for CHG_CONFIRM readings -> not charging.
//    3) FULL: charging and voltage >= CHG_FULL_V for CHG_FULL_HOLD_MS.
//
//  Public API (unchanged):
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
#include <esp_system.h>
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
#ifndef CHG_JUMP_V
#define CHG_JUMP_V 0.10f        // voltage rise above the baseline that means "charger plugged in" (V)
#endif
#ifndef CHG_DROP_V
#define CHG_DROP_V 0.10f        // voltage fall below the baseline that means "charger removed" (V)
#endif
#ifndef CHG_CONFIRM
#define CHG_CONFIRM 3           // so many readings in a row (= seconds) must agree
#endif
#ifndef CHG_BASE_NEW
#define CHG_BASE_NEW 0.05f      // baseline speed (0.05 = about 20 s time constant)
#endif
#ifndef CHG_FULL_V
#define CHG_FULL_V 4.17f        // at or above this (while charging) the cell is nearly full
#endif
#ifndef CHG_FULL_HOLD_MS
#define CHG_FULL_HOLD_MS 30000  // ... for this long => "full"
#endif
#ifndef CHG_FULL_HYST
#define CHG_FULL_HYST 0.07f     // "full" ends when the voltage is this far below CHG_FULL_V
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
static int chgMv = 0;              // not used any more (kept so old code that mentions it still compiles)
static float chgBase = 0;          // slow baseline of the raw battery voltage
static bool chgBaseOk = false;     // baseline has a first value
static uint8_t chgUpCnt = 0;       // readings in a row that are CHG_JUMP_V above the baseline
static uint8_t chgDnCnt = 0;       // readings in a row that are CHG_DROP_V below the baseline
static uint8_t chgLowCnt = 0;      // readings in a row that end the "full" state
static uint32_t chgFullSince = 0;  // when the voltage went >= CHG_FULL_V (0 = not now)
static bool gChargerBoot = false;  // true: the board started because USB was connected (no button press)

// The charge state survives a restart / crash in RTC memory (the pin test is wrong after those).
#define CHG_RTC_MAGIC 0xC4A6E501UL
RTC_NOINIT_ATTR static uint32_t rtcChgMagic;
RTC_NOINIT_ATTR static uint8_t rtcChgState;

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

// True when the board was started by the charger (no button press at boot).
inline bool chargerOnBoot() {
  return gChargerBoot;
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

// ---------- charger detection (from the battery voltage) -------------------------------

// Change the charge state (and remember it in RTC memory for a restart).
inline void chgSet(ChargeState s, const char* why) {
  if (s != chgState) {
    DBG(POWER, "charger: %s -> %s (%s)", chgState == CHG_CHARGING ? "CHARGING" : chgState == CHG_FULL ? "FULL" : "no USB",
        s == CHG_CHARGING ? "CHARGING" : s == CHG_FULL ? "FULL" : "no USB", why);
  }
  chgState = s;
  rtcChgState = (uint8_t)s;
  rtcChgMagic = CHG_RTC_MAGIC;
}

// Called with every new RAW battery voltage (about once a second).
inline void chargerDetect(float v, uint32_t now) {
  if (!chgBaseOk) {                           // first reading: start the baseline here
    chgBase = v;
    chgBaseOk = true;
    return;
  }
  float d = v - chgBase;                      // + = voltage went up, - = voltage went down

  if (chgState == CHG_NONE) {
    if (d >= CHG_JUMP_V) {                    // sudden rise: charger plugged in?
      if (++chgUpCnt >= CHG_CONFIRM) {        // it stayed up for a few seconds => yes
        chgUpCnt = 0;
        chgFullSince = 0;
        chgBase = v;
        chgSet(CHG_CHARGING, "voltage jump");
      }
      return;                                 // baseline is FROZEN while a jump is pending
    }
    chgUpCnt = 0;
    chgBase += d * CHG_BASE_NEW;              // slow follow (discharge is only a few mV per minute)
    return;
  }

  // charging or full
  if (d <= -CHG_DROP_V) {                     // sudden fall: charger removed?
    if (++chgDnCnt >= CHG_CONFIRM) {
      chgDnCnt = 0;
      chgFullSince = 0;
      chgBase = v;
      chgSet(CHG_NONE, "voltage drop");
    }
    return;
  }
  chgDnCnt = 0;
  chgBase += d * CHG_BASE_NEW;                // follow the slow rise of the charge

  if (chgState == CHG_CHARGING) {
    if (v >= CHG_FULL_V) {
      if (!chgFullSince) chgFullSince = now ? now : 1;
      else if (now - chgFullSince >= CHG_FULL_HOLD_MS) chgSet(CHG_FULL, "voltage high and steady");
    } else chgFullSince = 0;
  } else {                                    // CHG_FULL
    if (v < CHG_FULL_V - CHG_FULL_HYST) {     // voltage sank: charger gone (or recharge not started yet)
      if (++chgLowCnt >= CHG_CONFIRM) {
        chgLowCnt = 0;
        chgBase = v;
        chgSet(CHG_NONE, "full ended");       // a new jump (recharge) sets CHARGING again
      }
    } else chgLowCnt = 0;
  }
}

// Divider is connected directly to the battery (no MOSFET), so just read it every BAT_READ_MS.
inline void batteryService(uint32_t now) {
  if (batLast != 0 && now - batLast < BAT_READ_MS) return;
  float pinMv = (float)batteryReadPinMv();
  float v = pinMv * BAT_DIV * BAT_CAL / 1000.0f;   // BAT_DIV = 1.47 for 47k + 100k
  // first value is taken as is; later values are smoothed
  vbat = (batLast == 0) ? v : vbat * (1.0f - BAT_FILTER_NEW) + v * BAT_FILTER_NEW;
  batLast = now ? now : 1;                          // never 0 (0 means "no reading yet")
  chargerDetect(v, now);                            // charger state from the RAW voltage
  DBG(POWER, "battery: pin=%.0fmV raw=%.3fV filtered=%.3fV base=%.3fV -> %d%% (%s)", pinMv, v, vbat, chgBase,
      batteryPercent(), chgState == CHG_CHARGING ? "charging" : chgState == CHG_FULL ? "full" : "battery");
}

// ---------- start / stop ---------------------------------------------------------------

inline void powerBegin() {
  // 1) Find out how we started. This must happen BEFORE we drive PIN_POWER.
  esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_DEEPSLEEP) {
    gChargerBoot = false;                     // woken by the button, but USB must be present (we only sleep on USB)
    chgSet(CHG_CHARGING, "deep-sleep wake (USB present)");
  } else if (rr != ESP_RST_POWERON && rr != ESP_RST_BROWNOUT && rtcChgMagic == CHG_RTC_MAGIC && rtcChgState <= CHG_FULL) {
    gChargerBoot = false;                     // restart / crash: the pin test is not reliable, use the saved state
    chgSet((ChargeState)rtcChgState, "restored after restart");
  } else {                                    // real power-up: read the pin
    pinMode(PIN_POWER, INPUT_PULLDOWN);
    delay(2);                                 // let the wire settle
    bool pressed = (digitalRead(PIN_POWER) == HIGH);
    gChargerBoot = !pressed;                  // no button => the charger woke the board
    chgSet(pressed ? CHG_NONE : CHG_CHARGING, pressed ? "boot: button pressed" : "boot: no button => USB");
  }

  powerPinIdle();                             // hold the latch
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT, ADC_11db); // measure up to ~3.1 V at the pin
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
  rtcChgState = (uint8_t)chgState;            // keep the charge state over the restart
  rtcChgMagic = CHG_RTC_MAGIC;
  delay(100);
  ESP.restart();
  while (true) delay(1000);
}

// ---------- main loop part (called every ~1 ms) ----------------------------------------

inline void powerTick() {
  if (gPowerOffing) return;                   // shutting down: do not touch the pin
  uint32_t now = millis();
  batteryService(now);                        // also runs the charger detection

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