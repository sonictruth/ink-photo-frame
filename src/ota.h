#pragma once

#include <Arduino.h>

// Reads the OTA button's current level. Call once in setup(), before anything
// else touches its pin.
bool otaButtonPressed();

// Connects to WiFi and serves OTA firmware/filesystem updates until idle timeout
// or WiFi failure. Restarts the device on a successful update, so this only
// returns on failure or timeout.
void enterOtaMode();
