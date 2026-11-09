# Phoniebox ESP32

Classic ESP32 board that turns XiaoZhi into a Phoniebox-style RFID music
player. Present an RFID tag and the matching folder or file on the SD card is
played. A small web interface manages tag assignments and playback.

## Hardware

| Function | Signal | GPIO |
| --- | --- | --- |
| Shared SPI | SCK | 18 |
| Shared SPI | MOSI | 23 |
| Shared SPI | MISO | 19 |
| SD card | CS | 5 |
| MFRC522 / RC522 | CS | 21 |
| MFRC522 / RC522 | RST | 17 |
| MAX98357A | BCLK | 27 |
| MAX98357A | LRC / WS | 26 |
| MAX98357A | DIN | 25 |
| INMP441 | SCK | 14 |
| INMP441 | WS | 32 |
| INMP441 | SD | 34 |
| OLED | SDA | 4 |
| OLED | SCL | 15 |
| Next button | to GND | 16 |
| Previous button | to GND | 2 |
| Play / pause button | to GND | 33 |
| Chat / Wi-Fi config | BOOT | 0 |

Notes:

- Use a 3.3 V supply for the SD card and the RC522. The MAX98357A may be
  powered from 5 V; share ground with the ESP32.
- GPIO 34 is input-only, which is fine for the microphone data line.
- The OLED uses SDA 4 and SCL 15, matching the `bread-compact-esp32` board.
  GPIO 15 is a strapping pin, but the OLED's I2C pull-ups keep it high during
  boot. These two pins are therefore not available for buttons or the RC522
  reset line.
- The OLED is optional. If it does not respond the firmware continues
  without a display. Both SSD1306 and SH1106 128x64 panels are supported
  through separate build variants; they use the same I2C wiring.
- The 4 MB build keeps the standard 4 MB partition table. The 8 MB build uses
  `partitions/v2/8m.csv`.

## SD card layout

The card must be FAT32 and is mounted at `/sdcard`. Any folder can be assigned
to a tag. Supported files: MP3, WAV, FLAC, M4A, AAC and OGG.

```
/sdcard
  /music
    /bedtime
      01 - Goodnight.mp3
      02 - Sleep Tight.mp3
    /animals
      cat.mp3
  single-song.mp3
```

Assigning a folder plays its tracks in alphabetical order. Assigning a file
plays just that file.

## First use

1. Flash the firmware. The device starts the normal XiaoZhi Wi-Fi setup; use
   the BOOT button as usual to enter Wi-Fi configuration mode.
2. Join the same network and open `http://<device-ip>:8080/` in a browser.
   Port 8080 is used so the Phoniebox web UI does not conflict with the
   XiaoZhi Wi-Fi configuration portal on port 80. The IP address is shown by
   XiaoZhi's usual activation flow.
3. Present an unassigned tag. Its UID appears as "last scanned card".
4. Choose a folder or file and a friendly name, then save.
5. Present the tag again to play.

Tag assignments are stored in NVS, so they survive reboots and firmware
updates.

## Web API

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/` | Web interface |
| GET | `/api/status` | Player, storage, RFID and tag status |
| GET | `/api/tags` | List tag assignments |
| POST | `/api/tags` | `{"uid":"...","path":"/sdcard/...","name":"..."}` |
| DELETE | `/api/tags?uid=...` | Remove an assignment |
| GET | `/api/fs?path=/sdcard` | Browse folders and files |
| POST | `/api/play` | `{"path":"/sdcard/..."}` |
| POST | `/api/control` | `{"action":"pause"\|"resume"\|"toggle"\|"next"\|"previous"\|"stop"\|"volume","volume":0..100}` |

## Build

```sh
source $IDF_PATH/export.sh
python scripts/build.py phoniebox-esp32 --name esp32-phoniebox
python scripts/build.py phoniebox-esp32 --name esp32-phoniebox-sh1106
python scripts/build.py phoniebox-esp32 --name esp32-phoniebox-8mb
python scripts/build.py phoniebox-esp32 --name esp32-phoniebox-8mb-sh1106
```

Use the `-sh1106` variants for 1.3" SH1106 OLED modules and the 8 MB
variants for boards with 8 MB flash. Both display variants use the same I2C
pins (SDA 4, SCL 15).

## Behaviour with XiaoZhi

Music pauses automatically while the voice assistant is connecting, listening,
speaking or notifying, and stays paused afterwards. Press the play button to
resume. The BOOT button keeps its normal XiaoZhi behaviour.
