# Talking Directly to the PMD2X: the SPI Command Tester

*A minimal, serial-first ESP32C6 sketch for sending raw PMD2 SPI commands by
hand — built for debugging the link to the PMD2X, not for daily use.*

Alongside the wireless XIAO Dashboard, this repo carries a second, much
smaller sketch: the **SPI Command Tester**. Where the Dashboard gives you
charts, history, and a polished web UI, the Command Tester gives you a
bare command line over USB serial (plus a bare-bones web console) for
poking the PMD2X's SPI interface one command at a time. If you're
debugging the SPI link itself, or just want to see the raw bytes a command
returns, this is the tool to reach for.

It's carried over as-is from the `pmd2x` branch of
[ElmorLabs/PMD2-FW](https://github.com/ElmorLabs/PMD2-FW/tree/pmd2x/Arduino/PMD2_XIAO_ESP32C6_SPI_Master),
and lives in this repo at
[`PMD2_XIAO_ESP32C6_SPI_Master`](https://github.com/ElmorLabs-P1/pmd2x-xiao/tree/main/PMD2_XIAO_ESP32C6_SPI_Master).

## What it's for

- **Raw command access** — send any of the PMD2 protocol's commands
  (`welcome`, `vendor`, `uid`, `device`, `config`, `sensor`, `reset`) and
  see exactly what comes back, byte for byte.
- **Two ways in** — a USB serial console, or a bare HTML page served from
  the ESP32C6 itself, with a single text box and a scrolling log.
- **No validity checks** — unlike the Dashboard, it doesn't try to guess
  whether a reading looks plausible. Whatever the PMD2X sends back, good
  or garbage, gets printed as-is. That's a feature here: it's meant for
  seeing the truth of the link, not a polished view of it.

If the Dashboard is your daily driver, the Command Tester is your
multimeter.

## Building and flashing it with the Arduino IDE

1. **Install the ESP32 board package**, if you haven't already for the
   Dashboard sketch:
   - *File → Preferences → Additional Boards Manager URLs*, add:
     `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - *Tools → Board → Boards Manager*, search "esp32", install the
     Espressif package.

2. **Get the sketch.** Clone or download
   [`ElmorLabs-P1/pmd2x-xiao`](https://github.com/ElmorLabs-P1/pmd2x-xiao)
   and open
   `PMD2_XIAO_ESP32C6_SPI_Master/PMD2_XIAO_ESP32C6_SPI_Master.ino`.

3. **Add your WiFi credentials.** This sketch connects to WiFi on boot (for
   its optional web console) and will sit there waiting if it can't. Near
   the top of the file, edit these two lines with your network's SSID and
   password:
   ```cpp
   const char* WIFI_SSID = "xxx";
   const char* WIFI_PASS = "xxx";
   ```

4. **Pick the board and port**: *Tools → Board → esp32 → XIAO_ESP32C6*,
   then *Tools → Port* and select the ESP32C6's serial port.

5. **Partition scheme**: *Tools → Partition Scheme*, any default scheme
   works — this sketch is small and doesn't need OTA space.

6. **Upload.** Click the arrow button. The IDE compiles and flashes over
   the selected port.

7. **Open the Serial Monitor** at 115200 baud. You'll see the boot log and
   a command list, and you can type commands straight into the input box.

## Trying it out

Once it's running, type `help` into the Serial Monitor for the full command
list. The one you'll reach for most is `sensor`:

```
sensor
```

This prints the PMD2X's full sensor frame: supply voltage, chip
temperature, both 12VHPWR thermistors, total power, and every individual
voltage/current/power channel. If the PMD2X is alive and the SPI link is
healthy, you'll see real numbers. If you see all zeros across the board —
`Vdd=0 mV`, `Total=0 W`, every channel reading `0.000`  — that's a strong
signal the PMD2X's SPI responder has gotten stuck, not that anything is
wrong with the ESP32 side. A full power-cycle of the PMD2X (not just the
ESP32) is the known fix for that today.

You can also reach the same console from a browser: once connected to
WiFi, the sketch's serial log prints an IP address — open that in a
browser for a minimal web page with a text box and a live-updating log,
useful if you'd rather not tie up a USB serial connection.

## Useful commands

- `welcome` — reads the PMD2's product name string.
- `vendor` — vendor ID, product ID, and the PMD2X's own firmware version.
- `uid` — the PMD2X MCU's 12-byte hardware UID.
- `device` / `config` — reads back the device configuration summary.
- `sensor` / `once` — a single full sensor reading.
- `start` / `stop` — streams a `sensor` reading every 250 ms, handy for
  watching values change in real time.
- `reset` — tells the PMD2X to reboot.

It's deliberately low-level. If something about the SPI link looks wrong
from the Dashboard, this is the tool to confirm it with — no dashboard
polish standing between you and the actual bytes.
