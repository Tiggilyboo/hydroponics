# Hydroponics

My powersupply firmware for ESP32 C6

- 3x 24v grow LED strips (dimmable via PWM)
- 12v buck for: 1x 12v PC fan (4 pin control with PWM) 
- 5v buck for: ESP32 on board

## Hardware mapping

| Function | ESP32-C6 SuperMini | Connection |
|---|---|---|
| LED 1 | GPIO18 | 3.3 V-controlled MOSFET module 1 trigger |
| LED 2 | GPIO19 | MOSFET module 2 trigger |
| LED 3 | GPIO20 | MOSFET module 3 trigger |
| Fan tach | GPIO5 | Directly to fan tach; **internal pull-up enabled** |
| Fan PWM | GPIO4 | 220 ohm resistor to 2N7000 gate, 100k gate-to-GND pulldown |
| Common GND | GND | DC negative and both non-isolated buck converter returns |
| Supply | 5V input | MINI560 fixed 5 V buck, **not 3V3 pin** |

Fan PWM: 12 V remains powered; 2N7000 source to GND, drain to fan pin 4; fan pin 1 GND, pin 2 +12 V, pin 3 TACH, pin 4 PWM.
The external transistor **inverts** the GPIO waveform; `fanSetDutyPercent()` compensates.

## Linux prerequisites (no host ESP-IDF required)

The official `espressif/idf` image includes ESP-IDF, build tools, CMake, compilers and Python.
**The Zigbee library is an IDF Component Manager dependency** declared in `main/idf_component.yml`; it downloads during the first build (resolved from version in `dependencies.lock` and vendored under `managed_components/espressif__esp-zigbee-lib/`).

To verify:

```sh
docker version
make --version
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

## Build step by step

Run from this directory:

```sh
make build      # downloads Zigbee SDK component on first run
```

Equivalent underlying command:

```sh
docker run --rm --init --user "$(id -u):$(id -g)" \
  -e HOME=/tmp -e IDF_GIT_SAFE_DIR=/project \
  -v "$PWD:/project" -w /project \
  espressif/idf:v5.5.4 idf.py build
```

Changes to IDF configuration:

```sh
make menuconfig
```

`CMakeLists.txt` is the real ESP-IDF build entrypoint. The Makefile is only a **Docker wrapper** around `idf.py`; it is intentionally short.

`sdkconfig.defaults` is read **only when the generated `sdkconfig` is absent**. After editing it, delete `sdkconfig` (or run `make init`) before the next build — otherwise the stale values persist silently. Current settings: `CONFIG_ZB_ZCZR=y` is the Zigbee Coordinator/Router choice (`CONFIG_ZB_ZR` is not a symbol in SDK 2.x and is rejected with a Kconfig warning), and `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` matches the 4 MB flash of this SuperMini.

## Flash, monitor, erase over USB

```sh
make flash PORT=/dev/ttyACM0
```

*Monitor logs:*

```sh
make monitor PORT=/dev/ttyACM0
```

Exit IDF monitor with `Ctrl+]`. The Makefile passes only the selected serial device into Docker and adds its host device group. The image is linked for 4 MB flash; `build/flash_args` places bootloader at `0x0`, partition table at `0x8000`, OTA data at `0xf000` and the app at `0x20000`, and `make flash` writes those regions only — it does not clear Zigbee pairing state. If the USB serial device disappears/re-enumerates during reset, or Docker permissions block access, retry manually in download mode or use an ESP-IDF/esptool installation on the host to flash the generated `build/` artifacts. Don't indiscriminately use `--privileged` to work around access issues.

*Erase:*

If the device refuses to join after a previous unrelated firmware image, you may need to clear Zigbee pairing data.

```sh
make erase PORT=/dev/ttyACM0
```
