#!/usr/bin/env python3
"""Resizes images from ./images (root + subfolders, equally weighted) into the SPIFFS image and flashes it."""
import argparse
import os
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps

PROJECT_DIR = Path(__file__).resolve().parent
PARTITIONS_CSV = PROJECT_DIR / "partitions.csv"

# Matches the panel's native resolution in display.cpp (GxEPD2_750c_Z08).
LANDSCAPE_BOX = (800, 480)
PORTRAIT_BOX = (480, 800)

SOURCE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tif", ".tiff", ".webp"}


def find_pio() -> str:
    pio = shutil.which("pio")
    if pio:
        return pio
    fallback = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if fallback.exists():
        return str(fallback)
    sys.exit("Could not find the 'pio' executable; install PlatformIO or pass --pio <path>.")


def spiffs_partition_size(csv_path: Path) -> int:
    for line in csv_path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) >= 5 and fields[1] == "data" and fields[2] == "spiffs":
            return int(fields[4], 0)
    sys.exit(f"No data/spiffs partition found in {csv_path}")


def resize_for_panel(img: Image.Image) -> Image.Image:
    img = ImageOps.exif_transpose(img)
    img = img.convert("RGB")
    box = LANDSCAPE_BOX if img.width >= img.height else PORTRAIT_BOX
    img.thumbnail(box, Image.LANCZOS)
    return img


def extract_year(img: Image.Image):
    """Prefers EXIF DateTimeOriginal (when the photo was taken) over DateTime (often a save/edit time)."""
    try:
        exif = img.getexif()
    except Exception:
        return None
    if not exif:
        return None
    date_str = None
    try:
        date_str = exif.get_ifd(0x8769).get(0x9003)  # Exif IFD -> DateTimeOriginal
    except Exception:
        pass
    date_str = date_str or exif.get(0x0132)  # fallback: root IFD DateTime
    if not date_str:
        return None
    year = str(date_str)[:4]
    return year if year.isdigit() else None


def draw_year_caption(img: Image.Image, year: str) -> None:
    draw = ImageDraw.Draw(img)
    font_size = max(img.height // 18, 18)
    font = ImageFont.load_default(size=font_size)
    stroke_width = max(font_size // 10, 2)

    bbox = draw.textbbox((0, 0), year, font=font, stroke_width=stroke_width)
    x = (img.width - (bbox[2] - bbox[0])) // 2 - bbox[0]
    y = img.height - (bbox[3] - bbox[1]) - font_size // 2 - bbox[1]
    draw.text((x, y), year, font=font, fill="black", stroke_width=stroke_width, stroke_fill="white")


def fits_on_device(pio: str) -> bool:
    result = subprocess.run([pio, "run", "-e", "esp32dev", "-t", "buildfs"],
                             cwd=PROJECT_DIR, capture_output=True, text=True)
    return result.returncode == 0


def read_secrets_define(name: str) -> str:
    """Reads a #define's string value out of src/secrets.h, e.g. OTA_PASSWORD or OTA_HOSTNAME."""
    secrets = PROJECT_DIR / "src" / "secrets.h"
    if not secrets.exists():
        return ""
    match = re.search(rf'#define\s+{name}\s+"([^"]*)"', secrets.read_text())
    return match.group(1) if match else ""


def discover_buckets(images_dir: Path) -> dict:
    """Groups source images by bucket: "" for images_dir itself, else an immediate subfolder's name."""
    def images_in(d: Path):
        return [p for p in d.iterdir() if p.is_file() and p.suffix.lower() in SOURCE_EXTENSIONS]

    buckets = {}
    root_images = images_in(images_dir)
    if root_images:
        buckets[""] = root_images
    for entry in sorted(images_dir.iterdir()):
        if entry.is_dir():
            sub_images = images_in(entry)
            if sub_images:
                buckets[entry.name] = sub_images
    return buckets


def pick_candidates(buckets: dict, max_images):
    """Picks up to max_images (or all of them, if None): each pick is a random bucket, then a
    random not-yet-picked photo from it, so a small subfolder isn't drowned out by a large one."""
    pools = {name: list(paths) for name, paths in buckets.items()}
    total_sources = sum(len(v) for v in pools.values())
    limit = total_sources if max_images is None else min(max_images, total_sources)

    picked = []
    for _ in range(limit):
        nonempty = [name for name, pool in pools.items() if pool]
        folder = random.choice(nonempty)
        pool = pools[folder]
        src = pool.pop(random.randrange(len(pool)))
        picked.append((src, folder or "(root)"))
    return picked, total_sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images-dir", default="images", help="Source images (default: ./images)")
    parser.add_argument("--data-dir", default="data", help="Resized output, packed into SPIFFS (default: ./data)")
    parser.add_argument("--quality", type=int, default=45, help="JPEG quality, 1-95 (default: 45)")
    parser.add_argument("--max-images", type=int, default=80,
                         help="Randomly pick at most this many images before resizing (default: 80)")
    default_ota_host = f"{read_secrets_define('OTA_HOSTNAME')}.local"
    parser.add_argument("--port", default=None,
                         help="Upload over serial instead of OTA, to this port (passed to pio as --upload-port)")
    parser.add_argument("--ota-host", default=default_ota_host,
                         help=f"Device IP/hostname to OTA upload to (default: {default_ota_host})")
    parser.add_argument("--ota-password", default=None,
                         help="OTA auth password (default: OTA_PASSWORD from src/secrets.h)")
    parser.add_argument("--no-upload", action="store_true", help="Resize only; skip flashing")
    parser.add_argument("--pio", default=None, help="Path to the pio executable")
    args = parser.parse_args()

    images_dir = Path(args.images_dir)
    data_dir = Path(args.data_dir)

    if not images_dir.is_dir():
        sys.exit(f"{images_dir} does not exist")

    buckets = discover_buckets(images_dir)
    if not buckets:
        sys.exit(f"No images found in {images_dir} or its subfolders")

    if data_dir.exists():
        shutil.rmtree(data_dir)
    data_dir.mkdir(parents=True)

    partition_size = spiffs_partition_size(PARTITIONS_CSV)
    pio = args.pio or find_pio()

    candidates, total_sources = pick_candidates(buckets, args.max_images)
    print(f"Picked {len(candidates)} of {total_sources} source image(s).")

    included = []  # (src, dest, label) in the order added
    for i, (src, label) in enumerate(candidates, 1):
        with Image.open(src) as img:
            year = extract_year(img)
            resized = resize_for_panel(img)
        if year:
            draw_year_caption(resized, year)
        dest = data_dir / f"img{len(included):03d}.jpg"
        resized.save(dest, "JPEG", quality=args.quality, optimize=True, subsampling=2, progressive=False)
        included.append((src, dest, label))
        print(f"[{i}/{len(candidates)}] {src.name} -> {dest.name} ({dest.stat().st_size // 1024} KB)")

    # mkspiffs is the real authority on whether this fits; drop the most recently added image
    # and retry until it actually packs, rather than guessing a safety margin.
    unfit = []
    while included and not fits_on_device(pio):
        src, dest, _ = included.pop()
        dest.unlink()
        unfit.append(src)

    total_size = 0
    for src, dest, label in included:
        size = dest.stat().st_size
        total_size += size
        print(f"[{label}] {src.name} -> {dest.name} ({size} bytes)")

    print(f"\n{len(included)} images, {total_size} bytes total; SPIFFS partition is {partition_size} bytes.")

    if not included:
        sys.exit("Not even one image fits in the SPIFFS partition; lower --quality or grow it in "
                  f"{PARTITIONS_CSV.name}. Nothing was uploaded.")

    if unfit:
        print(f"Warning: partition full; {len(unfit)} of the {len(candidates)} picked image(s) were ignored: "
              f"{', '.join(p.name for p in unfit)}", file=sys.stderr)

    if args.no_upload:
        return

    env = os.environ.copy()
    if args.port:
        cmd = [pio, "run", "-e", "esp32dev", "-t", "uploadfs", "--upload-port", args.port]
    else:
        env["OTA_PASSWORD"] = args.ota_password or read_secrets_define("OTA_PASSWORD")
        cmd = [pio, "run", "-e", "esp32dev_ota", "-t", "uploadfs", "--upload-port", args.ota_host]
    subprocess.run(cmd, cwd=PROJECT_DIR, check=True, env=env)


if __name__ == "__main__":
    main()
