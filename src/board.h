// =====================================================================================
//  board.h  -  reads the 64 hall sensors (4 x 4 x 4 matrix, 12 pins)
//
//  Every sensor has 3 wires:
//     VCC  -> positive line i  (switched by a PNP transistor, pin HIGH = on via NPN driver)
//     GND  -> negative line j  (switched by an NPN transistor, pin HIGH = on)
//     OUT  -> data line k      (read directly, pull-up to 3V3)
//
//  Public API:
//     boardBegin()          set the pins up (call once)
//     boardScan()           call from the hardware task; scans ONE line per call
//     boardOccupancy()      64 bit mask, bit n = a magnet is on square n (a1 = bit 0)
//     boardRawChanged()     true once after the occupancy changed
//     boardReadRaw()        blocking full scan (only for test pages, not for hwTask)
// =====================================================================================
#pragma once
#include "gchess_config.h"
#include "debug.h"
#include "leds.h"   // hwToSq()

static portMUX_TYPE occMux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t occ64 = 0;            // published stable result (guarded by occMux)
static uint64_t rawBuild = 0;         // frame being assembled
static uint64_t rawLast = 0;          // previous complete frame
static uint8_t  rawStable = 0;        // identical frames in a row
static uint8_t  scanRow = 0;          // which positive line the next call scans (0..3)
static uint32_t scanLast = 0;         // start time of the last frame
static volatile uint32_t frameCount = 0;   // complete sensor frames since boot
static volatile bool occChanged = false;

// data line k, positive line i, negative line j  ->  chess square (0..63)
inline int sensorToSquare(int k, int i, int j) {
  int row = SENSOR_ROW(k, i, j);
  int col = SENSOR_COL(k, i, j);
  return hwToSq(row, col);
}

// All lines OFF: positive and negative pins LOW.
inline void boardBegin() {
  for (int n = 0; n < 4; n++) {
    pinMode(hallpin[0][n], OUTPUT);
    digitalWrite(hallpin[0][n], LOW);
    pinMode(hallpin[1][n], OUTPUT);
    digitalWrite(hallpin[1][n], LOW);
    pinMode(hallpin[2][n], INPUT);
  }
  scanRow = 0;
  rawBuild = 0;
  DBG(SENS, "hall matrix ready");
}

// Scan ONE positive line i (4 negative lines x settle time, well under 1 ms).
// Returns the 64-bit contribution of that line.
inline uint64_t boardScanLine(int i) {
  uint64_t occ = 0;
  digitalWrite(hallpin[0][i], HIGH);                  // row i gets power

  for (int j = 0; j < 4; j++) {
    digitalWrite(hallpin[1][j], HIGH);                // column j gets ground
    delayMicroseconds(HALL_SETTLE_US);

    for (int k = 0; k < 4; k++) {
      bool magnet = digitalRead(hallpin[2][k]) == (HALL_ACTIVE_LOW ? LOW : HIGH);
      if (magnet) occ |= 1ULL << sensorToSquare(k, i, j);
    }

    digitalWrite(hallpin[1][j], LOW);
  }

  digitalWrite(hallpin[0][i], LOW);
  return occ;
}

// Blocking full scan (4 lines). Do NOT call this from hwTask.
inline uint64_t boardReadRaw() {
  uint64_t occ = 0;
  for (int i = 0; i < 4; i++) occ |= boardScanLine(i);
  return occ;
}

// Called from the hardware task. Each call scans one line; after 4 calls a frame is
// complete, debounced and published.
inline void boardScan() {
  uint32_t now = millis();

  if (scanRow == 0) {
    if (now - scanLast < SENSOR_SCAN_MS) return;      // wait between frames
    scanLast = now;
    rawBuild = 0;
  }

  rawBuild |= boardScanLine(scanRow);
  scanRow++;
  if (scanRow < 4) return;                            // frame not finished yet
  scanRow = 0;
  frameCount++;                                       // NEW

  uint64_t raw = rawBuild;
  if (raw == rawLast) {
    if (rawStable < 255) rawStable++;
  } else {
    rawLast = raw;
    rawStable = 0;
  }

  if (rawStable == SENSOR_DEBOUNCE) {
    uint64_t before;
    portENTER_CRITICAL(&occMux);
    before = occ64;
    portEXIT_CRITICAL(&occMux);

    if (raw != before) {
      portENTER_CRITICAL(&occMux);
      occ64 = raw;
      portEXIT_CRITICAL(&occMux);
      occChanged = true;

      static char added[48], removed[48];             // static: keep them off the task stack
      dbgSquares(raw & ~before, added, sizeof(added));
      dbgSquares(before & ~raw, removed, sizeof(removed));
      DBG(SENS, "occupancy %016llx  pieces=%d  +[%s] -[%s]",
          (unsigned long long)raw, __builtin_popcountll(raw), added, removed);
    }
  }
}

// Latest stable result (safe from any task).
inline uint64_t boardOccupancy() {
  portENTER_CRITICAL(&occMux);
  uint64_t v = occ64;
  portEXIT_CRITICAL(&occMux);
  return v;
}

// true once after the occupancy changed
inline bool boardRawChanged() {
  bool c = occChanged;
  occChanged = false;
  return c;
}

// number of finished sensor frames (setup.h uses it for "stable for N frames")
inline uint32_t boardFrames() { return frameCount; }

// true once the first debounced occupancy has been published (about 100 ms after boot)
inline bool boardReady() { return frameCount >= SENSOR_DEBOUNCE + 3; }