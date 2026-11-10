# PMD2 XIAO ESP32C6 SPI Command Tester

These Arduino sketches match the PMD2 SPI3 slave implementation in
`Core/Src/spi_slave.c`.

## Status

This repo contains two Arduino sketches, each in its own folder (Arduino
requires a sketch's folder name to match its `.ino` filename):

- `PMD2_XIAO_ESP32C6_SPI_Master/` - the original serial-only command tester,
  copied as-is from the `pmd2x` branch of
  [ElmorLabs/PMD2-FW](https://github.com/ElmorLabs/PMD2-FW/tree/pmd2x)
  (`Arduino/PMD2_XIAO_ESP32C6_SPI_Master/`).
- `PMD2_XIAO_ESP32C6_Dashboard/` - a web dashboard build: same SPI command
  set, plus a live-updating telemetry page (power/temperature chart, CSV
  export, device info, reset), a JSON API, mDNS (`http://pmd2.local`), and
  a fallback access point if WiFi connect fails.

The PMD2 STM32 firmware itself (the SPI3 slave side) lives in the upstream
PMD2-FW repo, not here.

CI compiles both sketches on every push/PR (see `.github/workflows/ci.yml`);
no release workflow has been added.

## Wiring

- XIAO default `SPI SCK` -> PMD2 `SPI3_SCK` (PC10 on STM32)
- XIAO default `SPI MISO` -> PMD2 `SPI3_MISO` (PC11 on STM32)
- XIAO default `SPI MOSI` -> PMD2 `SPI3_MOSI` (PC12 on STM32)
- XIAO `GPIO17` -> PMD2 `SPI3_NSS` (PA15 on STM32)
- `GND` -> `GND`

Use 3.3 V logic levels.

## PMD2 SPI protocol used by this sketch

- SPI mode 0, MSB first
- master first sends a 1-byte command
- slave responds on a follow-up read transfer with command-specific payload
- payload read clocks dummy bytes (`0xFF`) from the master

Supported commands in firmware (`pmd2.h`):

- `CMD_WELCOME` (`0x00`) -> 15 bytes (`BuildData.ProductName`)
- `CMD_READ_VENDOR_DATA` (`0x01`) -> 3 bytes
- `CMD_READ_UID` (`0x02`) -> 12 bytes
- `CMD_READ_DEVICE_DATA` (`0x03`) -> `DeviceConfigStruct`
- `CMD_READ_SENSOR_VALUES` (`0x04`) -> `SensorStruct`
- `CMD_READ_CONFIG` (`0x06`) -> `DeviceConfigStruct`
- `CMD_RESET` (`0xF0`) -> no payload, device resets

## Board setup

1. Install ESP32 board support in Arduino IDE.
2. Select **Seeed XIAO ESP32C6**.
3. In the sketch folder you want to build (`PMD2_XIAO_ESP32C6_SPI_Master/`
   or `PMD2_XIAO_ESP32C6_Dashboard/`), copy `wifi_config.h.example` to
   `wifi_config.h` and fill in your WiFi SSID/password. `wifi_config.h` is
   gitignored so your credentials are never committed.
4. Open the sketch's `.ino` file.
5. Build and upload.
6. Open Serial Monitor at **115200 baud**.

## Serial commands

- `help`
- `welcome` - read product name
- `vendor` - read vendor/product/fw triplet
- `uid` - read 12-byte MCU UID
- `device` - read and print summary of `CMD_READ_DEVICE_DATA`
- `config` - read and print summary of `CMD_READ_CONFIG`
- `sensor` - read and print latest sensor summary
- `once` - same as `sensor`
- `start` - continuous sensor sampling every 250 ms
- `stop`
- `reset` - send reset command

## Notes

- The sketch removes the legacy fixed 16-byte vendor-pattern frame test.
- `device` and `config` currently return the same payload in the current
  `spi_slave.c` implementation.