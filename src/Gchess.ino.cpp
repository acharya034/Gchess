# 1 "C:\\Users\\Lenovo\\AppData\\Local\\Temp\\tmp5es4fmpj"
#include <Arduino.h>
# 1 "C:/Users/Lenovo/Documents/PlatformIO/Projects/hall sensor teaster/src/Gchess.ino"
# 28 "C:/Users/Lenovo/Documents/PlatformIO/Projects/hall sensor teaster/src/Gchess.ino"
#include "gchess_config.h"
#include "debug.h"
#include "events.h"
#include "storage.h"
#include "analytics.h"
#include "sound.h"
#include "power.h"
#include "leds.h"
#include "board.h"
#include "gfx.h"
#include "ui.h"
#include "game.h"
#include "net.h"
#include "pages.h"
#include "setup.h"
#include "ota.h"
#include <esp_bt.h>
static void goodbye();
static void hwTask(void*);
static void uiTask(void*);
void setup();
void loop();
#line 47 "C:/Users/Lenovo/Documents/PlatformIO/Projects/hall sensor teaster/src/Gchess.ino"
static void goodbye() {
  tft.setBrightness(gSet.tftBright);
  tft.fillScreen(T.bg);
  logoMedium(120, 130);
  gfxText(120, 190, "See you soon!", 2, T.fg, TD_TC);
  gfxText(120, 224, "Saving your data...", 1, T.dim, TD_TC);
}


static void hwTask(void*) {
  for (;;) {
    boardScan();
    soundTick();
    ledsTick();
    powerTick();
    analyticsTick();
    vTaskDelay(1);
  }
}


static void uiTask(void*) {
  uint32_t lastReport = 0;
  const uint32_t bootAt = millis();
  for (;;) {
    uiTouchPump();
    uiPumpEvents();

    if (!gBootChecked && boardReady() && millis() - bootAt > (uint32_t)SENSOR_DEBOUNCE * SENSOR_SCAN_MS * 2) {
      uint64_t o = boardOccupancy();
      gBootCheck = setupBootCheck(o);
      gBootChecked = true;
      DBG(SETUP, "boot check: %d (0=matches, 1=restorable, 2=unknown)  sensors=%d saved=%d diff=%d",
          (int)gBootCheck, __builtin_popcountll(o), __builtin_popcountll(gLb.occ), __builtin_popcountll(o ^ gLb.occ));
    }
    lastBoardTick();
    otaUiTick();
    if (setupActive()) setupTick(boardOccupancy());
    else physTick();
    if (!gScreenOff && gModal == MD_NONE && gPages[gCur].tick) gPages[gCur].tick();


    if (millis() - lastReport > 10000) {
      lastReport = millis();
      dbgHeap("ui");
      DBG(SYS, "stack left (words): ui=%u hw=? page=%s wifi=%d games(on board)=%d", (unsigned)uxTaskGetStackHighWaterMark(nullptr), gPages[gCur].name, (int)WiFi.status(), (int)(physLive() != nullptr));
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

void setup() {
  powerBegin();
  dbgBegin();
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  esp_bt_controller_disable();
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  DBG(SYS, "==== %s v%s booting, reset reason %d, cpu %d MHz ====", GC_NAME, GC_VERSION, (int)esp_reset_reason(), (int)getCpuFrequencyMhz());
  dbgHeap("boot");
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  storageBegin();
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  otaBegin();
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  analyticsBegin();
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  soundBegin();
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  lastBoardLoad();
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  ledsBegin();
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  boardBegin();
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  gfxBegin();
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  gBeforeOff = goodbye;
  DBG(SYS, "heap after display: free %u largest %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());


  qNet = xQueueCreate(16, sizeof(NetCmd));
  qUi = xQueueCreate(24, sizeof(UiEvt));
  gNetMux = xSemaphoreCreateMutex();
  memset(gNetG, 0, sizeof(gNetG));
  memset(&gNetX, 0, sizeof(gNetX));

  otaSlimBoot();

  gamesInit();
  pagesBegin();
  if (!gSet.wifiOn) netCmd(NC_WIFI_ENABLE, "", "", 0);


  gCur = PG_INTRO;
  analyticsPage(PG_INTRO);
  gPages[PG_INTRO].enter();
  gPages[PG_INTRO].draw();

  netBegin();
  xTaskCreatePinnedToCore(hwTask, "hw", 4096, nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(uiTask, "ui", 8192, nullptr, 3, nullptr, 0);
  dbgHeap("after start");
}

void loop() {
  vTaskDelete(nullptr);
}