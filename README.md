# Xtestatistics — Reading Statistics for CrossPoint Reader

A fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) that adds persistent reading statistics with streak tracking and a calendar heatmap. Works on **XTEINK X3 / X4** and other CrossPoint-compatible devices.

## What it does

- **On-device Statistics screen** — a new "Statistics" item in the home menu shows current streak, today's minutes, total, average, last 7 days bars, and a monthly calendar.
- **Automatic session tracking** — every reading session is saved to the SD card (`/ReadingStats/sessions.jsonl`) with start/end time, duration, book path, and progress. No manual input needed.
- **Web plugin** — an SD-card plugin provides a detailed web dashboard (open in a browser while the device is in File Transfer mode). Full calendar heatmap, streak/record, per-book breakdown, and 7-day bar chart.

## Screenshots

*(Add screenshots of the device screen and web plugin here.)*

## Installation

### 1. Build the firmware

```bash
git clone --recursive https://github.com/Eklim-s/Xtestatistics
cd Xtestatistics
pio run -e default
```

### 2. Flash to your device

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 .pio/build/default/firmware.bin
```

For X4 Pro / ESP32-S3 devices:
```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 .pio/build/default/firmware.bin
```

Or use the web flasher at [crosspointreader.com](https://crosspointreader.com/#flash-tools) — select "Custom .bin" and upload `firmware.bin`.

### 3. Install the web plugin (optional)

Copy the plugin folder to your SD card:

```
sd-plugins/reading-stats-viewer/  →  SD:/plugins/reading-stats-viewer/
```

The dashboard is available at `http://crosspoint.local/plugin?name=reading-stats-viewer&file=index.html` while in File Transfer mode.

## Data format

Sessions are stored at `/ReadingStats/sessions.jsonl` on the SD card, one JSON object per line:

```json
{"s":1734212345,"e":1734212895,"d":550,"b":"/Books/MyBook.epub","doc":"abc123...","sb":1200,"eb":2400}
```

| Field | Description |
|---|---|
| `s` | Session start (Unix epoch seconds) |
| `e` | Session end (Unix epoch seconds) |
| `d` | Duration in seconds |
| `b` | Book path on SD card |
| `doc` | KOReader-compatible document ID |
| `sb` | Start progress in basis points (0–10000) |
| `eb` | End progress in basis points (0–10000) |

## Upstream updates

This fork tracks CrossPoint Reader's `develop` branch. To merge upstream changes:

```bash
git remote add upstream https://github.com/crosspoint-reader/crosspoint-reader.git
git fetch upstream
git merge upstream/develop
```

## Building from scratch

Requirements:
- Python 3.8+
- [PlatformIO Core](https://platformio.org/) (`pip install platformio`)
- `clang-format` 21 (optional, for code formatting)

```bash
git clone --recursive https://github.com/Eklim-s/Xtestatistics
cd Xtestatistics
pio run -e default
```

The compiled firmware is at `.pio/build/default/firmware.bin`.

## License

MIT — same as CrossPoint Reader. See [LICENSE](LICENSE).

## Credits

Built on [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) — open-source e-reader firmware by the CrossPoint community.