#include "ota.h"
#include "display.h"
#include "storage.h"
#include "secrets.h"

#include <WiFi.h>
#include <ArduinoOTA.h>

namespace
{
  // Board's BOOT button: pulled to GND when pressed, internal pull-up holds it high
  // otherwise. Only read during the normal wake-up boot sequence (no dedicated wake
  // source) to keep power use down between scheduled wakes.
  constexpr gpio_num_t BUTTON_PIN = GPIO_NUM_0;
  constexpr int STATUS_LED_PIN = 2; // mirrors STATUS_LED in main.cpp

  constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
  constexpr uint32_t IDLE_TIMEOUT_MS = 5UL * 60UL * 1000UL;
}

bool otaButtonPressed()
{
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(10);
  return digitalRead(BUTTON_PIN) == LOW;
}

void enterOtaMode()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS)
  {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    showText("OTA mode\nWiFi connect failed");
    delay(2000);
    return;
  }

  showText("OTA mode\nReady at " + WiFi.localIP().toString());

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD) > 0)
  {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  uint32_t lastActivity = millis();

  // Only the filesystem (images) partition is updatable: partitions.csv has a single
  // "ota_0" app slot, so a firmware update would target the partition this code is
  // currently running from. Restart immediately to refuse it before anything is written.
  ArduinoOTA.onStart([]()
                      {
    if (ArduinoOTA.getCommand() != U_SPIFFS)
    {
      ESP.restart();
    }
    unmountStorage(); });

  // No display work in onStart/onProgress: espota only waits ~10s for the device to
  // connect back after onStart runs, and a full e-paper refresh takes longer than that.
  ArduinoOTA.onProgress([&lastActivity](unsigned int progress, unsigned int total)
                         {
    lastActivity = millis();
    digitalWrite(STATUS_LED_PIN, (progress / 4096) % 2); });

  ArduinoOTA.onEnd([]()
                    { ESP.restart(); });

  ArduinoOTA.onError([](ota_error_t error) {});

  ArduinoOTA.begin();

  while (millis() - lastActivity < IDLE_TIMEOUT_MS)
  {
    ArduinoOTA.handle();
    delay(10);
  }
}
