#pragma once

#include <Arduino.h>

bool setupStorage();

// Unmounts the filesystem; call before an OTA filesystem update rewrites the partition.
void unmountStorage();

// Returns the full path ("/xxx.jpg") of a random JPEG in storage, or "" if none exist.
String pickRandomImage();
