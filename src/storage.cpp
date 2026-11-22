#include "storage.h"
#include <SPIFFS.h>
#include <esp_system.h>

bool setupStorage()
{
  return SPIFFS.begin(false);
}

void unmountStorage()
{
  SPIFFS.end();
}

static bool isJPEG(const String &name)
{
  return name.endsWith(".jpg") || name.endsWith(".jpeg");
}

String pickRandomImage()
{
  int count = 0;
  File dir = SPIFFS.open("/");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile())
  {
    if (isJPEG(String(f.name())))
      count++;
  }

  if (count == 0)
    return String();

  int pick = esp_random() % count;
  int i = 0;
  String result;
  dir = SPIFFS.open("/");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile())
  {
    if (!isJPEG(String(f.name())))
      continue;
    if (i == pick)
    {
      result = f.name();
      break;
    }
    i++;
  }
  return result;
}
