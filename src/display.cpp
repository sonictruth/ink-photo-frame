#define USE_HSPI_FOR_EPD
#define ENABLE_GxEPD2_GFX 0

#include "display.h"
#include <GxEPD2_3C.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <SPIFFS.h>
#include <JPEGDEC.h>
#include <esp_heap_caps.h>

// Same panel wiring as web2ink-esp32: CS=15 DC=27 RST=26 BUSY=25,
// HSPI remapped to SCK=13 MISO=12 MOSI=14 CS=15.
#define GxEPD2_DRIVER_CLASS GxEPD2_750c_Z08

// Small page buffer for text only; images allocate their own planes from heap.
#define MAX_DISPLAY_BUFFER_SIZE 8000ul
#define MAX_HEIGHT(EPD) (EPD::HEIGHT <= (MAX_DISPLAY_BUFFER_SIZE / 2) / (EPD::WIDTH / 8) ? EPD::HEIGHT : (MAX_DISPLAY_BUFFER_SIZE / 2) / (EPD::WIDTH / 8))

GxEPD2_3C<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)> display(GxEPD2_DRIVER_CLASS(/*CS=*/15, /*DC=*/27, /*RST=*/26, /*BUSY=*/25));

#if defined(USE_HSPI_FOR_EPD)
SPIClass hspi(HSPI);
#endif

static bool displayReady = false;

void setupDisplay()
{
  if (displayReady)
    return;
  displayReady = true;

#if defined(USE_HSPI_FOR_EPD)
  hspi.begin(13, 12, 14, 15); // remap hspi for EPD (swap pins)
  display.epd2.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
#endif

  display.init(115200);
  display.setRotation(1);
}

void showText(String text)
{
  Serial.print("[screen] ");
  Serial.println(text);
  setupDisplay();
  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);

  // Split on '\n' and center each line separately: GFX's own newline handling
  // resets the cursor to x=0, not back to a centered x, so a multi-line string
  // can't be centered as a single block.
  String lines[4];
  int lineCount = 0;
  int start = 0;
  while (lineCount < 4)
  {
    int nl = text.indexOf('\n', start);
    bool last = nl < 0;
    lines[lineCount++] = last ? text.substring(start) : text.substring(start, nl);
    if (last)
      break;
    start = nl + 1;
  }

  int16_t tbx, tby;
  uint16_t tbw, tbh;
  display.getTextBounds("Ag", 0, 0, &tbx, &tby, &tbw, &tbh);
  uint16_t lineHeight = tbh + 8;
  int16_t refTby = tby;
  uint16_t blockTop = (display.height() - lineHeight * lineCount) / 2;

  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    for (int i = 0; i < lineCount; i++)
    {
      display.getTextBounds(lines[i], 0, 0, &tbx, &tby, &tbw, &tbh);
      uint16_t x = ((display.width() - tbw) / 2) - tbx;
      uint16_t y = blockTop + lineHeight * i - refTby;
      display.setCursor(x, y);
      display.print(lines[i]);
    }
  } while (display.nextPage());
}

void displayPowerOff()
{
  if (displayReady)
    display.powerOff();
}

// ---- JPEG -> dithered black/white/red rendering ----

namespace
{

  struct RGB
  {
    uint8_t r, g, b;
  };

  // Approximation; the panel's real red is closer to orange.
  const RGB PALETTE[3] = {
      {255, 255, 255}, // white
      {0, 0, 0},       // black
      {255, 0, 0},     // red
  };

  // drawImage() bypasses setRotation(); buffers stay in native orientation.
  const int16_t NATIVE_W = GxEPD2_DRIVER_CLASS::WIDTH;
  const int16_t NATIVE_H = GxEPD2_DRIVER_CLASS::HEIGHT;

  JPEGDEC jpeg;
  File jpegFile;

  void *jpegFileOpen(const char *filename, int32_t *size)
  {
    jpegFile = SPIFFS.open(filename);
    if (!jpegFile)
      return nullptr;
    *size = jpegFile.size();
    return &jpegFile;
  }

  void jpegFileClose(void *handle)
  {
    if (handle)
      ((File *)handle)->close();
  }

  int32_t jpegFileRead(JPEGFILE *jFile, uint8_t *buffer, int32_t length)
  {
    return ((File *)jFile->fHandle)->read(buffer, length);
  }

  int32_t jpegFileSeek(JPEGFILE *jFile, int32_t position)
  {
    return ((File *)jFile->fHandle)->seek(position);
  }

  uint8_t *blackBuf = nullptr; // packed 1bpp, bit=0 means black, sized to canvas
  uint8_t *colorBuf = nullptr; // packed 1bpp, bit=0 means red, sized to canvas
  uint16_t rowBytes = 0;
  int16_t canvasW = 0, canvasH = 0;
  int16_t imgOffsetX = 0, imgOffsetY = 0;
  int16_t imgW = 0, imgH = 0;

  // Stucki needs the current row plus the next two; padded +-2 for the kernel's x offsets.
  int32_t *errRow[3] = {nullptr, nullptr, nullptr};

  uint16_t *bandBuf = nullptr; // holds one decoded MCU row-band, full image width
  int bandY = -1;              // image-relative y of the band being accumulated
  int bandFilled = 0;          // columns filled so far in the current band
  int bandRows = 0;            // row count of the current band

  // True for an unrotated 800x480 landscape placement; false for the usual 480x800 portrait one.
  bool nativeMode = false;

  // Mirrors the rotation switch in GxEPD2_3C::drawPixel.
  inline void mapToNative(int lx, int ly, int &nx, int &ny)
  {
    if (nativeMode)
    {
      nx = lx;
      ny = ly;
      return;
    }
    switch (display.getRotation())
    {
    case 1:
      nx = NATIVE_W - ly - 1;
      ny = lx;
      break;
    case 2:
      nx = NATIVE_W - lx - 1;
      ny = NATIVE_H - ly - 1;
      break;
    case 3:
      nx = ly;
      ny = NATIVE_H - lx - 1;
      break;
    default:
      nx = lx;
      ny = ly;
      break;
    }
  }

  inline void clearBit(uint8_t *buf, int lx, int ly)
  {
    int nx, ny;
    mapToNative(lx, ly, nx, ny);
    buf[(uint32_t)ny * rowBytes + nx / 8] &= ~(0x80 >> (nx % 8));
  }

  void ditherRow(const uint16_t *src, int y)
  {
    int32_t *cur = errRow[0];
    int32_t *next1 = errRow[1];
    int32_t *next2 = errRow[2];

    // Accumulated unscaled (divided by 42 once on read) to avoid per-tap truncation.
    auto spread = [&](int32_t *row, int x, int dx, int weight, int errR, int errG, int errB)
    {
      int idx = (x + dx + 2) * 3;
      row[idx + 0] += errR * weight;
      row[idx + 1] += errG * weight;
      row[idx + 2] += errB * weight;
    };

    for (int x = 0; x < imgW; x++)
    {
      uint16_t p = src[x];
      int r = ((p >> 11) & 0x1F) * 255 / 31;
      int g = ((p >> 5) & 0x3F) * 255 / 63;
      int b = (p & 0x1F) * 255 / 31;

      r = constrain(r + cur[(x + 2) * 3 + 0] / 42, 0, 255);
      g = constrain(g + cur[(x + 2) * 3 + 1] / 42, 0, 255);
      b = constrain(b + cur[(x + 2) * 3 + 2] / 42, 0, 255);

      int best = 0;
      long bestDist = LONG_MAX;
      for (int i = 0; i < 3; i++)
      {
        long dr = r - PALETTE[i].r;
        long dg = g - PALETTE[i].g;
        long db = b - PALETTE[i].b;
        long dist = dr * dr + dg * dg + db * db;
        if (dist < bestDist)
        {
          bestDist = dist;
          best = i;
        }
      }

      int outX = imgOffsetX + x;
      int outY = imgOffsetY + y;
      if (best == 1)
        clearBit(blackBuf, outX, outY);
      else if (best == 2)
        clearBit(colorBuf, outX, outY);

      int errR = r - PALETTE[best].r;
      int errG = g - PALETTE[best].g;
      int errB = b - PALETTE[best].b;

      spread(cur, x, 1, 8, errR, errG, errB);
      spread(cur, x, 2, 4, errR, errG, errB);
      spread(next1, x, -2, 2, errR, errG, errB);
      spread(next1, x, -1, 4, errR, errG, errB);
      spread(next1, x, 0, 8, errR, errG, errB);
      spread(next1, x, 1, 4, errR, errG, errB);
      spread(next1, x, 2, 2, errR, errG, errB);
      spread(next2, x, -2, 1, errR, errG, errB);
      spread(next2, x, -1, 2, errR, errG, errB);
      spread(next2, x, 0, 4, errR, errG, errB);
      spread(next2, x, 1, 2, errR, errG, errB);
      spread(next2, x, 2, 1, errR, errG, errB);
    }

    // row y is fully consumed now; rotate it to become the future "+2" row
    memset(cur, 0, (imgW + 4) * 3 * sizeof(int32_t));
    errRow[0] = next1;
    errRow[1] = next2;
    errRow[2] = cur;
  }

  // JPEGDEC's internal pixel buffer is narrower than most images, so bands are accumulated here until a full row is ready to dither.
  int jpegDraw(JPEGDRAW *pDraw)
  {
    if (pDraw->y != bandY)
    {
      bandY = pDraw->y;
      bandFilled = 0;
      bandRows = pDraw->iHeight;
    }

    int w = pDraw->iWidthUsed;
    if (pDraw->x + w > imgW)
      w = imgW - pDraw->x;
    if (w > 0)
    {
      for (int r = 0; r < pDraw->iHeight; r++)
      {
        memcpy(&bandBuf[r * imgW + pDraw->x], &pDraw->pPixels[r * pDraw->iWidth], w * sizeof(uint16_t));
      }
    }
    bandFilled = pDraw->x + w;

    if (bandFilled >= imgW)
    {
      for (int r = 0; r < bandRows; r++)
      {
        ditherRow(&bandBuf[r * imgW], bandY + r);
      }
    }

    return 1;
  }

  struct ImageBuffers
  {
    ~ImageBuffers()
    {
      free(blackBuf);
      free(colorBuf);
      free(bandBuf);
      free(errRow[0]);
      free(errRow[1]);
      free(errRow[2]);
      blackBuf = colorBuf = nullptr;
      bandBuf = nullptr;
      errRow[0] = errRow[1] = errRow[2] = nullptr;
    }

    bool alloc(int16_t nativeW, int16_t nativeH, int16_t iw)
    {
      rowBytes = nativeW / 8; // native panel width is always byte-aligned
      size_t planeSize = (size_t)rowBytes * nativeH;
      size_t errSize = (size_t)(iw + 4) * 3 * sizeof(int32_t);

      blackBuf = (uint8_t *)malloc(planeSize);
      colorBuf = (uint8_t *)malloc(planeSize);
      bandBuf = (uint16_t *)malloc((size_t)iw * 16 * sizeof(uint16_t));
      errRow[0] = (int32_t *)malloc(errSize);
      errRow[1] = (int32_t *)malloc(errSize);
      errRow[2] = (int32_t *)malloc(errSize);

      if (!blackBuf || !colorBuf || !bandBuf || !errRow[0] || !errRow[1] || !errRow[2])
        return false;

      memset(blackBuf, 0xFF, planeSize);
      memset(colorBuf, 0xFF, planeSize);
      memset(errRow[0], 0, errSize);
      memset(errRow[1], 0, errSize);
      memset(errRow[2], 0, errSize);
      return true;
    }
  };

} // namespace

bool showImageJPEG(const char *path)
{
  setupDisplay();

  canvasW = display.width();
  canvasH = display.height();

  if (!SPIFFS.exists(path))
  {
    Serial.println(F("[image] file not found"));
    showText("No image found");
    return false;
  }

  if (!jpeg.open(path, jpegFileOpen, jpegFileClose, jpegFileRead, jpegFileSeek, jpegDraw))
  {
    Serial.println(F("[image] open failed"));
    showText("Bad JPEG file");
    jpeg.close();
    return false;
  }

  imgW = jpeg.getWidth();
  imgH = jpeg.getHeight();

  if (imgW <= canvasW && imgH <= canvasH)
  {
    nativeMode = false;
  }
  else if (imgW <= NATIVE_W && imgH <= NATIVE_H)
  {
    nativeMode = true; // landscape source shown unrotated, matching the panel's native size
  }
  else
  {
    Serial.printf("[image] too large: %dx%d\n", imgW, imgH);
    String msg = "Image too large: " + String(imgW) + "x" + String(imgH);
    showText(msg);
    jpeg.close();
    return false;
  }

  if (jpeg.getJPEGType() == JPEG_MODE_PROGRESSIVE)
  {
    Serial.println(F("[image] progressive JPEG not supported"));
    showText("Progressive JPEG not supported");
    jpeg.close();
    return false;
  }

  imgOffsetX = ((nativeMode ? NATIVE_W : canvasW) - imgW) / 2;
  imgOffsetY = ((nativeMode ? NATIVE_H : canvasH) - imgH) / 2;
  bandY = -1;
  bandFilled = 0;
  bandRows = 0;

  Serial.printf("[image] free heap=%u largest block=%u\n", ESP.getFreeHeap(),
                 heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  ImageBuffers buffers;
  if (!buffers.alloc(NATIVE_W, NATIVE_H, imgW))
  {
    Serial.println(F("[image] out of memory"));
    showText("Out of memory");
    jpeg.close();
    return false;
  }

  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  bool decoded = jpeg.decode(0, 0, 0);
  if (!decoded)
  {
    Serial.printf("[image] decode failed, error=%d\n", jpeg.getLastError());
  }
  jpeg.close();

  if (!decoded)
  {
    showText("JPEG decode failed");
    return false;
  }

  display.drawImage(blackBuf, colorBuf, 0, 0, NATIVE_W, NATIVE_H);
  return true;
}
