# ink-photo-frame

A battery-friendly e-paper photo frame built on an ESP32. It wakes every 12 hours, picks a random JPEG from flash, dithers it to the panel's black/white/red palette, shows it, and goes back to deep sleep. Photos are pre-resized on your computer and pushed to the device over Wi-Fi (OTA) or USB.

## Features

- 7.5" 3-color (black / white / red) e-paper panel, 800×480 (GxEPD2 `GxEPD2_750c_Z08`)
- Baseline JPEG decoding with Stucki error-diffusion dithering to the 3-color palette
- Random image selection from SPIFFS on each wake
- EXIF year caption burned into each photo by the host-side prep script
- ~12 hour deep-sleep cycle; LED blink codes indicate the reset reason
- Brownout / crash protection: repeated brownouts or panics skip the refresh and sleep instead
- OTA updates for the image partition (firmware OTA is deliberately refused; see below)

## Hardware

- ESP32 dev board
- 7.5" black/white/red e-paper panel supported by `GxEPD2_750c_Z08`

Panel wiring (also noted in `src/display.cpp`):

| Signal | GPIO |
| ------ | ---- |
| CS     | 15   |
| DC     | 27   |
| RST    | 26   |
| BUSY   | 25   |
| SCK (HSPI, remapped) | 13 |
| MISO (HSPI, remapped) | 12 |
| MOSI (HSPI, remapped) | 14 |

- **BOOT button (GPIO 0)** — held down during power-on to enter OTA mode
- **Status LED (GPIO 2)** — blink codes on boot:
  - 1 long blink: power-on
  - 2 blinks: woke from deep sleep (normal refresh)
  - 4 fast blinks: brownout
  - 6 fast blinks: other reset (panic, watchdog, …)

## Getting started

### Prerequisites

- [PlatformIO](https://platformio.org/) (`pio` on your PATH, or in `~/.platformio/penv/bin`)
- Python 3 with Pillow for the image prep script: `pip install -r requirements.txt`

### Configuration

Copy the secrets template and fill in your values:

```sh
cp src/secrets.h.example src/secrets.h
```

- `WIFI_SSID` / `WIFI_PASSWORD` — used only in OTA mode
- `OTA_HOSTNAME` — mDNS hostname advertised by the device
- `OTA_PASSWORD` — password for OTA uploads (leave `""` to disable; not recommended)

### Build and first flash

The first firmware flash must be over serial (the device refuses firmware OTA):

```sh
pio run -e esp32dev -t upload          # or -t uploadfs / upload with --upload-port
pio device monitor                     # 115200 baud
```

### Loading images

Put photos in `./images` — the root folder and each subfolder are treated as equally weighted "buckets", so a small album isn't drowned out by a big one. Supported sources: jpg, png, bmp, gif, tif, webp.

```sh
python flash_images.py                          # resize + upload over OTA (uses secrets.h)
python flash_images.py --port /dev/cu.usbserial-XXXX   # upload over serial instead
python flash_images.py --no-upload              # resize into ./data only
```

Useful options: `--max-images N` (default 80), `--quality N` (default 45), `--images-dir`, `--data-dir`, `--ota-host`, `--ota-password`.

The script:

1. Picks a random sample from each bucket, resized to fit 800×480 (landscape) or 480×800 (portrait)
2. Draws the EXIF capture year in the bottom corner
3. Saves baseline JPEGs into `data/` (progressive JPEG is not supported by the decoder)
4. Packs the SPIFFS image, dropping the newest picks until everything fits the partition
5. Uploads it to the device (`uploadfs`) over OTA or serial

### Updating images later

Once the frame is on your network, hold the BOOT button while powering it on (or pressing reset). It shows `OTA mode / Ready at <ip>` on screen and waits 5 minutes. Then re-run `python flash_images.py`.

## How it works

- **Lifecycle** (`src/main.cpp`): deep sleep for 12 hours → wake → LED reset-reason code → optional OTA mode → pick a random JPEG from SPIFFS → render → sleep. Brownout restarts are counted in RTC memory; three in a row (or a panic/watchdog reset) put the device back to sleep without touching the display.
- **Rendering** (`src/display.cpp`): JPEGDEC decodes the file while two 1-bpp planes are filled from heap — one for black, one for red — with Stucki error diffusion mapping each pixel to the nearest palette color, then both planes are pushed to the panel.
- **Storage** (`src/storage.cpp`): SPIFFS holds the resized JPEGs; the picker counts them, picks one at random via `esp_random()`.
- **OTA** (`src/ota.cpp`): ArduinoOTA, filesystem partition only. `partitions.csv` has a single `ota_0` app slot with no room for an A/B firmware update, so a firmware upload request makes the device restart rather than overwrite the partition it's running from. Image updates use the `esp32dev_ota` environment (`pio run -e esp32dev_ota -t uploadfs --upload-port <ip>`), which `flash_images.py` drives for you.

## Project layout

```
├── platformio.ini        # PlatformIO config (esp32dev + esp32dev_ota environments)
├── partitions.csv        # 1.25 MB app slot + ~2.7 MB SPIFFS for images
├── flash_images.py       # Host-side: resize, caption, pack, upload images
├── images/               # Your source photos (not committed)
├── data/                 # Resized output packed into SPIFFS (generated)
└── src/
    ├── main.cpp          # Boot, sleep cycle, LED codes, OTA entry point
    ├── display.cpp/.h    # Panel driver, JPEG decode + dithering
    ├── storage.cpp/.h    # SPIFFS mount, random image picker
    ├── ota.cpp/.h        # Wi-Fi + ArduinoOTA (filesystem updates)
    ├── secrets.h         # Your Wi-Fi/OTA credentials (gitignored)
    └── secrets.h.example # Template for secrets.h
```
