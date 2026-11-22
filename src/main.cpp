#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "display.h"
#include "storage.h"
#include "ota.h"

#define SLEEP_HOURS 12
#define STATUS_LED 2
#define BROWNOUT_MAGIC 0x5A5A5A5A

// Debug only: forces OTA mode on every boot, ignoring the button. Set back to 0 for normal use.
#define DEBUG_FORCE_OTA 0

// RTC memory keeps its value across brownout restarts but holds garbage
// after a real power-on, so the magic value tells them apart.
RTC_NOINIT_ATTR uint32_t brownoutMagic;
RTC_NOINIT_ATTR uint32_t brownoutCount;

void goToSleep(uint64_t hours = 0)
{
  displayPowerOff();
  delay(1000);

  digitalWrite(STATUS_LED, LOW);

  if (hours > 0)
  {
    esp_sleep_enable_timer_wakeup(hours * 3600ULL * 1000000ULL);
  }
  esp_deep_sleep_start();
}

void flashLed(int times, uint32_t onMs = 200, uint32_t offMs = 300)
{
  for (int i = 0; i < times; i++)
  {
    digitalWrite(STATUS_LED, HIGH);
    delay(onMs);
    digitalWrite(STATUS_LED, LOW);
    delay(offMs);
  }
}

void flashResetReason(esp_reset_reason_t reason)
{
  switch (reason)
  {
  case ESP_RST_POWERON:
    flashLed(1, 800, 200);
    break;
  case ESP_RST_BROWNOUT:
    flashLed(4, 100, 100);
    break;
  case ESP_RST_DEEPSLEEP:
    flashLed(2, 200, 300);
    break;
  default:
    flashLed(6, 100, 100);
    break;
  }
}

void setup()
{
  WiFi.mode(WIFI_OFF);
  Serial.begin(115200);
  Serial.println(F("Wake"));

  pinMode(STATUS_LED, OUTPUT);

  esp_reset_reason_t reason = esp_reset_reason();
  flashResetReason(reason);

  // Survives the brownout restart; magic guards against garbage after a real power-on.
  if (brownoutMagic != BROWNOUT_MAGIC || reason != ESP_RST_BROWNOUT)
  {
    brownoutMagic = BROWNOUT_MAGIC;
    brownoutCount = 0;
  }
  if (reason == ESP_RST_BROWNOUT && ++brownoutCount >= 3)
  {
    goToSleep();
  }

  if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT)
  {
    goToSleep();
  }

  if (DEBUG_FORCE_OTA || otaButtonPressed())
  {
    enterOtaMode();
  }

  esp_wifi_stop();

  if (setupStorage())
  {
    String path = pickRandomImage();
    if (path.length() > 0)
    {
      showImageJPEG(path.c_str());
    }
    else
    {
      showText("No images found");
    }
  }
  else
  {
    showText("Storage error");
  }

  flashLed(2); // debug: about to sleep

  goToSleep(SLEEP_HOURS);
}

void loop()
{
}
