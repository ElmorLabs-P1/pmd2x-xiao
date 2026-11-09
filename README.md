# PMD2 XIAO ESP32C6 SPI Command Tester

This Arduino sketch matches the PMD2 SPI3 slave implementation in
`Core/Src/spi_slave.c`.

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
3. Open `PMD2_XIAO_ESP32C6_SPI_Master.ino`.
4. Build and upload.
5. Open Serial Monitor at **115200 baud**.

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