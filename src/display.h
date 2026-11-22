#pragma once

#include <Arduino.h>

void setupDisplay();
void showText(String text);
void displayPowerOff();

// Renders a JPEG from flash storage dithered to black/white/red; shows an error message instead on failure.
bool showImageJPEG(const char *path);
