#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include "wifi_config.h"   // optional compiled-in defaults: WIFI_SSID / WIFI_PASS

WebServer server(80);
String webOutputBuffer;
Preferences g_prefs;

// ====== Configuration ======
static constexpr uint32_t SERIAL_BAUD = 115200;
static constexpr uint32_t SPI_CLOCK_HZ = 1000000;
static constexpr uint8_t PMD2_CS_PIN = 17;
static constexpr uint32_t STREAM_INTERVAL_MS = 1000;   // verbose console "start" stream
static constexpr uint32_t SAMPLE_INTERVAL_MS = 1000;   // dashboard sampling
static constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
static constexpr size_t WEB_LOG_MAX = 4096;

static const char* MDNS_NAME = "pmd2";                 // http://pmd2.local
static const char* AP_SSID = "PMD2-Dashboard";         // fallback access point
static const char* AP_PASS = "pmd2pmd2";               // min. 8 characters

// ====== BLE WiFi provisioning ======
// Lets a phone (e.g. nRF Connect, LightBlue, or a custom app) write WiFi
// credentials over BLE instead of requiring them to be compiled in. Stored
// in NVS via Preferences so they survive reboots and re-flashing of the
// sketch itself.
static const char* BLE_DEVICE_NAME = "PMD2-Setup";
static const char* BLE_SERVICE_UUID       = "7a0a1000-0000-1000-8000-00805f9b34fb";
static const char* BLE_CHAR_SSID_UUID     = "7a0a1001-0000-1000-8000-00805f9b34fb";
static const char* BLE_CHAR_PASS_UUID     = "7a0a1002-0000-1000-8000-00805f9b34fb";
static const char* BLE_CHAR_APPLY_UUID    = "7a0a1003-0000-1000-8000-00805f9b34fb";
static const char* BLE_CHAR_STATUS_UUID   = "7a0a1004-0000-1000-8000-00805f9b34fb";

static NimBLECharacteristic* g_bleStatusChar = nullptr;
static String g_pendingSsid;
static String g_pendingPass;
static volatile bool g_bleApplyRequested = false;

class ProvisioningCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (c->getUUID().equals(NimBLEUUID(BLE_CHAR_SSID_UUID))) {
      g_pendingSsid = c->getValue();
    } else if (c->getUUID().equals(NimBLEUUID(BLE_CHAR_PASS_UUID))) {
      g_pendingPass = c->getValue();
    } else if (c->getUUID().equals(NimBLEUUID(BLE_CHAR_APPLY_UUID))) {
      g_bleApplyRequested = true;
    }
  }
};

void bleSetStatus(const char* s) {
  if (!g_bleStatusChar) return;
  g_bleStatusChar->setValue(s);
  g_bleStatusChar->notify();
}

// Starts a BLE GATT service for WiFi provisioning. Safe to run alongside
// WiFi/AP mode: the ESP32C6's BLE and WiFi radios share the antenna but the
// Arduino core handles coexistence, no extra wiring needed.
void startProvisioningBle() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEServer* bleServer = NimBLEDevice::createServer();
  NimBLEService* svc = bleServer->createService(BLE_SERVICE_UUID);

  auto* callbacks = new ProvisioningCallbacks();

  NimBLECharacteristic* ssidChar = svc->createCharacteristic(
      BLE_CHAR_SSID_UUID, NIMBLE_PROPERTY::WRITE);
  ssidChar->setCallbacks(callbacks);

  NimBLECharacteristic* passChar = svc->createCharacteristic(
      BLE_CHAR_PASS_UUID, NIMBLE_PROPERTY::WRITE);
  passChar->setCallbacks(callbacks);

  NimBLECharacteristic* applyChar = svc->createCharacteristic(
      BLE_CHAR_APPLY_UUID, NIMBLE_PROPERTY::WRITE);
  applyChar->setCallbacks(callbacks);

  g_bleStatusChar = svc->createCharacteristic(
      BLE_CHAR_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_bleStatusChar->setValue("idle");

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->start();
}

// Applies pending BLE-provisioned credentials: save to NVS, then reconnect
// WiFi using them. Called from loop() so it runs outside the BLE callback.
void applyPendingWifiCredentials() {
  g_bleApplyRequested = false;
  if (g_pendingSsid.length() == 0) {
    bleSetStatus("error: empty ssid");
    return;
  }

  g_prefs.putString("ssid", g_pendingSsid);
  g_prefs.putString("pass", g_pendingPass);
  bleSetStatus("connecting");

  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_pendingSsid.c_str(), g_pendingPass.c_str());

  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    dualPrint("BLE provisioning: WiFi connected, IP: " + WiFi.localIP().toString());
    bleSetStatus(("connected:" + WiFi.localIP().toString()).c_str());
  } else {
    dualPrint("BLE provisioning: WiFi connect failed, staying on AP");
    bleSetStatus("error: connect failed");
  }
}

// ====== PMD2 protocol ======
static constexpr uint8_t CMD_WELCOME = 0x00;
static constexpr uint8_t CMD_READ_VENDOR_DATA = 0x01;
static constexpr uint8_t CMD_READ_UID = 0x02;
static constexpr uint8_t CMD_READ_DEVICE_DATA = 0x03;
static constexpr uint8_t CMD_READ_SENSOR_VALUES = 0x04;
static constexpr uint8_t CMD_READ_CONFIG = 0x06;
static constexpr uint8_t CMD_RESET = 0xF0;
static constexpr uint8_t CMD_MCU_CMD = 0xF2;

static constexpr uint32_t MCU_CMD_KEY = 0x0BADC0DE;
static constexpr uint8_t MCU_CMD_RESET = 0x03;

static constexpr size_t DEVICE_STR_LEN = 32;
static constexpr size_t SENSOR_POWER_NUM = 16;
static constexpr size_t SENSOR_TS_NUM = 2;

#pragma pack(push, 1)
struct VendorDataStruct {
  uint8_t VendorId;
  uint8_t ProductId;
  uint8_t FwVersion;
};

struct PowerSensor {
  uint16_t Voltage;
  uint32_t Current;
  uint32_t Power;
};

struct SensorStruct {
  uint16_t Vdd;
  uint16_t Tchip;
  int16_t Ts[SENSOR_TS_NUM];
  PowerSensor PowerReadings[SENSOR_POWER_NUM];
  uint16_t EpsPower;
  uint16_t PciePower;
  uint16_t MbPower;
  uint16_t TotalPower;
  uint8_t Ocp[SENSOR_POWER_NUM];
};

struct CalibrationValueStruct {
  int16_t Offset;
  int16_t GainOffset;
};

struct CalibrationStruct {
  CalibrationValueStruct PowerReadingVoltage[SENSOR_POWER_NUM];
  CalibrationValueStruct PowerReadingCurrent[SENSOR_POWER_NUM];
};

// Enums in PMD2 firmware are stored as 32-bit int by GCC.
struct DeviceConfigStruct {
  uint8_t Version;
  uint16_t Crc;
  int32_t Average;
  int32_t OcpScale;
  int32_t DisplayRotation;
  uint8_t OcpPerChannel[SENSOR_POWER_NUM];
  CalibrationStruct Calibration;
};
#pragma pack(pop)

void printConfigSummary(const DeviceConfigStruct& cfg, const char* label);

static_assert(sizeof(VendorDataStruct) == 3, "VendorDataStruct size mismatch");
static_assert(sizeof(SensorStruct) == 192, "SensorStruct size mismatch");
static_assert(sizeof(DeviceConfigStruct) == 159, "DeviceConfigStruct size mismatch");

// ====== Console state ======
static bool g_streamEnabled = false;
static String g_inputLine;
static uint32_t g_lastStreamMs = 0;

// ====== Dashboard state ======
struct HistRec {
  uint16_t total, eps, pcie, mb;
  int16_t tsIn, tsOut;           // tenths of a degree, as reported by the PMD2
};

static constexpr size_t HIST_LEN = 300;                 // 5 minutes at 1 Hz
static HistRec g_hist[HIST_LEN];
static size_t g_histHead = 0;                           // next write slot
static size_t g_histCount = 0;

static SensorStruct g_sensor = {};
static bool g_sensorValid = false;
static bool g_sampling = true;
static uint32_t g_sampleSeq = 0;
static uint32_t g_lastSampleMs = 0;

static uint32_t g_statsStartMs = 0;
static uint32_t g_statsSamples = 0;
static double g_sumPower = 0;
static uint16_t g_peakPower = 0;
static double g_wattHours = 0;
static uint32_t g_lastEnergyMs = 0;
static bool g_energyPrimed = false;

// ====== Dual output helper (Serial + web console) ======
void dualPrint(const String& s) {
  Serial.println(s);
  webOutputBuffer += s + "\n";
  if (webOutputBuffer.length() > WEB_LOG_MAX) {
    webOutputBuffer.remove(0, webOutputBuffer.length() - (WEB_LOG_MAX * 3 / 4));
  }
}

// Helper: print a hex buffer to both Serial and web
void printHex(const uint8_t* data, size_t len) {
  String out;
  for (size_t i = 0; i < len; ++i) {
    if (i) out += " ";
    if (data[i] < 16) out += "0";
    out += String(data[i], HEX);
  }
  dualPrint(out);
}

// ====== SPI (verbose versions, used by the console commands) ======
void spiSendCommand(uint8_t cmd) {
  SPI.beginTransaction(SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PMD2_CS_PIN, LOW);
  SPI.transfer(cmd);
  digitalWrite(PMD2_CS_PIN, HIGH);
  SPI.endTransaction();

  dualPrint("Sent SPI command: 0x" + String(cmd, HEX));
}

void spiReadPayload(uint8_t* rx, size_t len) {
  uint8_t tx[256];
  if (len > sizeof(tx)) len = sizeof(tx);
  memset(tx, 0xFF, len);

  SPI.beginTransaction(SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PMD2_CS_PIN, LOW);
  SPI.transferBytes(tx, rx, len);
  digitalWrite(PMD2_CS_PIN, HIGH);
  SPI.endTransaction();

  dualPrint("Read SPI payload (" + String(len) + " bytes)");
}

void spiWritePayload(uint8_t* tx, size_t len) {
  uint8_t rx[256];
  if (len > sizeof(rx)) len = sizeof(rx);
  memset(rx, 0xFF, len);

  SPI.beginTransaction(SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PMD2_CS_PIN, LOW);
  SPI.transferBytes(tx, rx, len);
  digitalWrite(PMD2_CS_PIN, HIGH);
  SPI.endTransaction();

  dualPrint("Wrote SPI payload (" + String(len) + " bytes)");
}

void spiCommandRead(uint8_t cmd, uint8_t* rx, size_t len) {
  spiSendCommand(cmd);
  delay(1);
  spiReadPayload(rx, len);
  dualPrint("spiCommandRead complete for cmd=0x" + String(cmd, HEX));
}

void spiCommandWrite(uint8_t cmd, uint8_t* tx, size_t len) {
  spiSendCommand(cmd);
  delay(1);
  spiWritePayload(tx, len);
  dualPrint("spiCommandWrite complete for cmd=0x" + String(cmd, HEX));
}

// ====== SPI (quiet version, used by the dashboard: no logging, no String use) ======
// Same wire sequence as spiCommandRead(): command byte, 1 ms gap, payload read.
static void spiCommandReadRaw(uint8_t cmd, uint8_t* rx, size_t len) {
  uint8_t tx[256];
  if (len > sizeof(tx)) len = sizeof(tx);
  memset(tx, 0xFF, len);

  SPI.beginTransaction(SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PMD2_CS_PIN, LOW);
  SPI.transfer(cmd);
  digitalWrite(PMD2_CS_PIN, HIGH);
  SPI.endTransaction();

  delay(1);

  SPI.beginTransaction(SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PMD2_CS_PIN, LOW);
  SPI.transferBytes(tx, rx, len);
  digitalWrite(PMD2_CS_PIN, HIGH);
  SPI.endTransaction();
}

// ====== Console commands ======
void cmdReadWelcome() {
  uint8_t name[16] = {};
  spiCommandRead(CMD_WELCOME, name, sizeof(name));

  char text[17] = {};
  memcpy(text, name, sizeof(name));

  dualPrint("WELCOME: \"" + String(text) + "\"");
  dualPrint("Raw: ");
  printHex(name, sizeof(name));
}

void cmdReadVendor() {
  VendorDataStruct data = {};
  spiCommandRead(CMD_READ_VENDOR_DATA, reinterpret_cast<uint8_t*>(&data), sizeof(data));

  dualPrint("VendorData: VID=0x" + String(data.VendorId, HEX) +
            " PID=0x" + String(data.ProductId, HEX) +
            " FW=0x" + String(data.FwVersion, HEX));
}

void cmdReadUid() {
  uint8_t uid[12] = {};
  spiCommandRead(CMD_READ_UID, uid, sizeof(uid));

  dualPrint("UID:");
  printHex(uid, sizeof(uid));
}

void printConfigSummary(const DeviceConfigStruct& cfg, const char* label) {
  dualPrint(String(label) + ": ver=" + String(cfg.Version) +
            " crc=0x" + String(cfg.Crc, HEX) +
            " avg=" + String((long)cfg.Average) +
            " ocpScale=" + String((long)cfg.OcpScale) +
            " rotation=" + String((long)cfg.DisplayRotation));
}

void cmdReadConfig(bool useDeviceCmd) {
  DeviceConfigStruct cfg = {};
  const uint8_t cmd = useDeviceCmd ? CMD_READ_DEVICE_DATA : CMD_READ_CONFIG;
  spiCommandRead(cmd, reinterpret_cast<uint8_t*>(&cfg), sizeof(cfg));

  printConfigSummary(cfg, useDeviceCmd ? "DeviceData" : "Config");
}

void cmdReadSensor() {
  SensorStruct sensor = {};
  spiCommandRead(CMD_READ_SENSOR_VALUES, reinterpret_cast<uint8_t*>(&sensor), sizeof(sensor));

  dualPrint("Sensor:");
  dualPrint("  Vdd=" + String(sensor.Vdd) + " mV");
  dualPrint("  Tchip=" + String(sensor.Tchip));
  dualPrint("  TsIn=" + String(sensor.Ts[0] / 10.0f) + "*C TsOut=" + String(sensor.Ts[1] / 10.0f) + "*C");
  dualPrint("  Total=" + String(sensor.TotalPower) + " W");

  dualPrint("Rails:");
  dualPrint("  EPS=" + String(sensor.EpsPower) + " W");
  dualPrint("  PCIE=" + String(sensor.PciePower) + " W");
  dualPrint("  MB=" + String(sensor.MbPower) + " W");

  for (size_t i = 0; i < SENSOR_POWER_NUM; ++i) {
    const PowerSensor& ps = sensor.PowerReadings[i];
    float volts = ps.Voltage / 1000.0f;
    float amps  = ps.Current / 1000.0f;
    float watts = ps.Power   / 1000.0f;

    dualPrint("  Channel[" + String(i) + "]: " +
              String(volts, 3) + " V, " +
              String(amps, 3) + " A, " +
              String(watts, 3) + " W, " +
              "OCP=" + String(sensor.Ocp[i]));
  }
}

void cmdReset() {
  spiSendCommand(CMD_RESET);
  dualPrint("RESET command sent");
}

void cmdMcuCmd(uint8_t mcu_cmd) {
  uint8_t tx_buf[5];
  tx_buf[0] = MCU_CMD_KEY & 0xFF;
  tx_buf[1] = MCU_CMD_KEY >> 8;
  tx_buf[2] = MCU_CMD_KEY >> 16;
  tx_buf[3] = MCU_CMD_KEY >> 24;
  tx_buf[4] = mcu_cmd;

  spiCommandWrite(CMD_MCU_CMD, tx_buf, 5);
}

void sampleOnce() {
  cmdReadSensor();
}

void printHelp() {
  dualPrint("");
  dualPrint("PMD2 SPI command tester (XIAO ESP32C6)");
  dualPrint("Commands:");
  dualPrint("  help      - show this help");
  dualPrint("  welcome   - CMD_WELCOME");
  dualPrint("  vendor    - CMD_READ_VENDOR_DATA");
  dualPrint("  uid       - CMD_READ_UID");
  dualPrint("  device    - CMD_READ_DEVICE_DATA");
  dualPrint("  config    - CMD_READ_CONFIG");
  dualPrint("  sensor    - CMD_READ_SENSOR_VALUES");
  dualPrint("  mcu_cmd_reset    - CMD_MCU_CMD MCU_CMD_RESET");
  dualPrint("  once      - same as sensor");
  dualPrint("  start     - print sensor every " + String(STREAM_INTERVAL_MS) + " ms");
  dualPrint("  stop      - stop printing");
  dualPrint("  reset     - CMD_RESET");
  dualPrint("");
}

void handleCommand(const String& line) {
  webOutputBuffer = "";

  if (line.length() == 0) return;

  if (line.equalsIgnoreCase("help")) { printHelp(); return; }
  if (line.equalsIgnoreCase("once")) { sampleOnce(); return; }
  if (line.equalsIgnoreCase("welcome")) { cmdReadWelcome(); return; }
  if (line.equalsIgnoreCase("vendor")) { cmdReadVendor(); return; }
  if (line.equalsIgnoreCase("uid")) { cmdReadUid(); return; }
  if (line.equalsIgnoreCase("device")) { cmdReadConfig(true); return; }
  if (line.equalsIgnoreCase("config")) { cmdReadConfig(false); return; }
  if (line.equalsIgnoreCase("sensor")) { cmdReadSensor(); return; }
  if (line.equalsIgnoreCase("start")) { g_streamEnabled = true; dualPrint("Sensor stream: ON"); return; }
  if (line.equalsIgnoreCase("stop")) { g_streamEnabled = false; dualPrint("Sensor stream: OFF"); return; }
  if (line.equalsIgnoreCase("reset")) { cmdReset(); return; }
  if (line.equalsIgnoreCase("mcu_cmd_reset")) { cmdMcuCmd(MCU_CMD_RESET); return; }

  dualPrint("Unknown command: " + line);
  printHelp();
}

// ====== Dashboard sampling ======
void resetStats() {
  g_peakPower = 0;
  g_sumPower = 0;
  g_statsSamples = 0;
  g_wattHours = 0;
  g_energyPrimed = false;
  g_statsStartMs = millis();
}

// Reads one sensor frame quietly and updates history + running stats.
void sampleTick() {
  SensorStruct s;
  spiCommandReadRaw(CMD_READ_SENSOR_VALUES, reinterpret_cast<uint8_t*>(&s), sizeof(s));

  // Heuristic: a missing/unpowered PMD2 reads back all 0xFF or all 0x00.
  const bool allFF = (s.Vdd == 0xFFFF && s.Tchip == 0xFFFF);
  const bool all00 = (s.Vdd == 0 && s.Tchip == 0 && s.TotalPower == 0);
  g_sensorValid = !(allFF || all00);
  if (!g_sensorValid) return;

  g_sensor = s;
  g_sampleSeq++;

  const uint32_t now = millis();

  HistRec& h = g_hist[g_histHead];
  h.total = s.TotalPower;
  h.eps = s.EpsPower;
  h.pcie = s.PciePower;
  h.mb = s.MbPower;
  h.tsIn = s.Ts[0];
  h.tsOut = s.Ts[1];
  g_histHead = (g_histHead + 1) % HIST_LEN;
  if (g_histCount < HIST_LEN) g_histCount++;

  if (s.TotalPower > g_peakPower) g_peakPower = s.TotalPower;
  g_sumPower += s.TotalPower;
  g_statsSamples++;

  if (g_energyPrimed) {
    g_wattHours += (double)s.TotalPower * (double)(now - g_lastEnergyMs) / 3600000.0;
  }
  g_lastEnergyMs = now;
  g_energyPrimed = true;
}

// ====== Dashboard page ======
static const char PAGE_DASH[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PMD2 Telemetry</title>
<style>
:root{--bg:#0e1116;--card:#161b22;--line:#262d38;--tx:#e6e9ee;--mut:#9aa4b2;--acc:#4cc9f0;--ok:#5be39b;--bad:#ff8a8a;
--sans:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font-family:var(--sans)}
main{max-width:1200px;margin:0 auto;padding:24px 16px 40px;display:flex;flex-direction:column;gap:16px}
header{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:12px}
h1{margin:0;font-size:22px}
.host{font:13px var(--mono);color:var(--mut);margin-left:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:18px;min-width:0}
.lbl{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--mut);margin:0 0 8px}
.big{font:500 36px var(--mono);margin:0}
.u{font-size:15px;color:var(--mut);margin-left:6px}
.sub{font-size:13px;color:var(--mut);margin:6px 0 0}
.grid{display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(220px,1fr))}
.two{grid-template-columns:repeat(auto-fit,minmax(320px,1fr))}
.wide{grid-column:span 2}
@media(max-width:700px){.wide{grid-column:auto}}
.row{display:grid;grid-template-columns:52px 1fr 1fr 1fr 46px;gap:8px;align-items:center;padding:8px 0;border-bottom:1px solid #222a35;font:13px var(--mono);color:#cfd6df}
.nm{color:var(--mut)}.w{color:var(--tx)}
.pill{font:600 11px var(--sans);border-radius:6px;padding:3px 0;text-align:center;background:#12301f;color:var(--ok)}
.pill.trip{background:#3b1519;color:var(--bad)}
.btn{font:500 14px var(--sans);min-height:44px;padding:0 18px;border-radius:10px;border:1px solid #2f3846;background:#1c232d;color:var(--tx);cursor:pointer;display:inline-flex;align-items:center;text-decoration:none}
.btn.pri{background:var(--acc);border-color:var(--acc);color:#06222b}
.btn.danger{border-color:#5a2a2e;color:var(--bad)}
.bar{height:8px;border-radius:4px;background:#222a35;overflow:hidden}
.bar>div{height:100%;background:var(--acc);width:0}
.rail{display:flex;justify-content:space-between;font-size:14px;margin-bottom:6px}
.rail b{font:500 14px var(--mono)}
.live{display:flex;align-items:center;gap:8px;font-size:13px;color:var(--mut)}
.live i{width:8px;height:8px;border-radius:50%;background:#6b7685}
.live.on{color:var(--ok)}.live.on i{background:var(--ok)}
.legend{display:flex;gap:20px;font-size:13px;color:var(--mut);margin-top:8px}
#nodata{background:#3b1519;border:1px solid #5a2a2e;color:var(--bad);border-radius:10px;padding:12px 16px;font-size:14px}
dialog{background:var(--card);color:var(--tx);border:1px solid var(--line);border-radius:12px;max-width:520px;width:calc(100% - 32px)}
dialog::backdrop{background:rgba(0,0,0,.6)}
dialog pre{font:13px/1.6 var(--mono);white-space:pre-wrap;margin:0 0 16px}
[hidden]{display:none!important}
</style></head><body><main>

<header>
<div><h1 style="display:inline">PMD2 Telemetry</h1><span class="host" id="host"></span></div>
<div style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">
<span class="live" id="live"><i></i><span id="livetxt">Connecting</span></span>
<button class="btn pri" id="pause" type="button">Pause</button>
<a class="btn" href="/api/csv" download="pmd2_log.csv">Download CSV</a>
</div>
</header>

<div id="nodata" hidden>No valid data from the PMD2. Check the SPI wiring, chip select and power.</div>

<section class="grid">
<div class="card"><p class="lbl">Total power</p><p class="big" style="color:var(--acc)"><span id="tot">--</span><span class="u">W</span></p><p class="sub">Peak <span id="pk">--</span> W &middot; Avg <span id="av">--</span> W</p></div>
<div class="card"><p class="lbl">Energy</p><p class="big"><span id="wh">--</span><span class="u">Wh</span></p><p class="sub">Since reset &middot; <span id="el">--:--</span> elapsed</p></div>
<div class="card"><p class="lbl">Inlet / outlet</p><p class="big"><span id="tin">--</span><span class="u">&deg;C</span></p><p class="sub">Outlet <span id="tout">--</span> &deg;C &middot; &Delta; <span id="dt">--</span> &deg;C</p></div>
<div class="card"><p class="lbl">Supply</p><p class="big"><span id="vdd">--</span><span class="u">V Vdd</span></p><p class="sub">Chip <span id="tc">--</span></p></div>
</section>

<section class="grid two">
<div class="card wide">
<p class="lbl">Power, last 5 minutes</p>
<svg id="chart" viewBox="0 0 640 200" style="width:100%;height:auto;display:block" role="img" aria-label="Line chart of total and PCIe power over the last five minutes"></svg>
<div class="legend"><span><span style="color:var(--acc)">&#9473;</span> Total</span><span><span style="color:#6b7685">&#9473;</span> PCIe</span></div>
</div>
<div class="card" style="display:flex;flex-direction:column;gap:18px">
<p class="lbl" style="margin:0">Rails</p>
<div><div class="rail"><span>EPS</span><b><span id="r1">--</span> W</b></div><div class="bar"><div id="b1"></div></div></div>
<div><div class="rail"><span>PCIe</span><b><span id="r2">--</span> W</b></div><div class="bar"><div id="b2"></div></div></div>
<div><div class="rail"><span>Motherboard</span><b><span id="r3">--</span> W</b></div><div class="bar"><div id="b3"></div></div></div>
</div>
</section>

<section class="card">
<p class="lbl">Channels</p>
<div class="grid two" style="column-gap:32px;gap:0 32px"><div id="c1"></div><div id="c2"></div></div>
</section>

<footer style="display:flex;flex-wrap:wrap;gap:12px">
<button class="btn" id="info" type="button">Device info</button>
<button class="btn" id="stats" type="button">Reset stats</button>
<button class="btn danger" id="reset" type="button">Reset PMD2</button>
<a class="btn" href="/console">Console</a>
</footer>

<dialog id="dlg"><pre id="infotxt"></pre><form method="dialog"><button class="btn pri">Close</button></form></dialog>

</main>
<script>
const $=id=>document.getElementById(id),HL=300;
let hist={t:[],p:[]},lastSeq=-1,paused=false;
$('host').textContent=location.host;

let h1='',h2='';
for(let i=0;i<16;i++){
  const r='<div class="row"><span class="nm">CH '+String(i+1).padStart(2,'0')+'</span><span id="v'+i+'">--</span><span id="a'+i+'">--</span><span class="w" id="w'+i+'">--</span><span class="pill" id="o'+i+'">--</span></div>';
  if(i<8)h1+=r;else h2+=r;
}
$('c1').innerHTML=h1;$('c2').innerHTML=h2;

async function j(u,o){const r=await fetch(u,o);if(!r.ok)throw new Error(r.status);return r.json()}
function mmss(s){return String(Math.floor(s/60)).padStart(2,'0')+':'+String(s%60).padStart(2,'0')}

function draw(){
  const X0=40,W=590,Y0=20,H=150,n=hist.t.length;
  const mx=Math.max(100,Math.ceil(Math.max(...hist.t,...hist.p)*1.1/100)*100);
  let g='';
  for(let k=0;k<=3;k++){
    const y=Y0+H*k/3;
    g+='<line x1="40" y1="'+y+'" x2="630" y2="'+y+'" stroke="#262d38"/>';
    g+='<text x="4" y="'+(y+4)+'" fill="#9aa4b2" font-size="11">'+Math.round(mx*(1-k/3))+(k==0?' W':'')+'</text>';
  }
  g+='<text x="40" y="194" fill="#9aa4b2" font-size="11">-5 min</text><text x="596" y="194" fill="#9aa4b2" font-size="11">now</text>';
  const pts=a=>a.map((v,i)=>(X0+W*((HL-n+i)/(HL-1))).toFixed(1)+','+(Y0+H*(1-v/mx)).toFixed(1)).join(' ');
  if(n>1){
    g+='<polyline fill="none" stroke="#6b7685" stroke-width="2" points="'+pts(hist.p)+'"/>';
    g+='<polyline fill="none" stroke="#4cc9f0" stroke-width="2.5" points="'+pts(hist.t)+'"/>';
  }
  $('chart').innerHTML=g;
}

function render(s){
  $('nodata').hidden=!!s.ok;
  paused=s.paused;$('pause').textContent=paused?'Resume':'Pause';
  if(!s.ok)return;
  $('tot').textContent=s.total;$('pk').textContent=s.peak;$('av').textContent=Math.round(s.avg);
  $('wh').textContent=s.wh.toFixed(1);$('el').textContent=mmss(s.elapsed);
  $('tin').textContent=(s.tsin/10).toFixed(1);$('tout').textContent=(s.tsout/10).toFixed(1);
  $('dt').textContent=((s.tsout-s.tsin)/10).toFixed(1);
  $('vdd').textContent=(s.vdd/1000).toFixed(2);$('tc').textContent=s.tchip;
  [[s.eps,'1'],[s.pcie,'2'],[s.mb,'3']].forEach(([v,k])=>{
    $('r'+k).textContent=v;$('b'+k).style.width=Math.min(100,v/Math.max(1,s.total)*100)+'%';
  });
  s.ch.forEach(([v,i,p,o],k)=>{
    $('v'+k).textContent=(v/1000).toFixed(2)+' V';
    $('a'+k).textContent=(i/1000).toFixed(2)+' A';
    $('w'+k).textContent=(p/1000).toFixed(1)+' W';
    const e=$('o'+k);e.textContent=o?'OCP':'OK';e.className='pill'+(o?' trip':'');
  });
  if(s.seq!==lastSeq){
    lastSeq=s.seq;hist.t.push(s.total);hist.p.push(s.pcie);
    if(hist.t.length>HL){hist.t.shift();hist.p.shift()}
    draw();
  }
}

async function poll(){
  try{
    const s=await j('/api/sensor');render(s);
    $('live').className='live'+(s.paused?'':' on');
    $('livetxt').textContent=s.paused?'Paused':'Live \u00b7 1 Hz';
  }catch(e){
    $('live').className='live';$('livetxt').textContent='Offline';
  }
  setTimeout(poll,1000);
}

async function cmd(c){
  try{await j('/api/cmd',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'c='+c})}catch(e){}
}
$('pause').onclick=async()=>{await cmd(paused?'resume':'pause')};
$('stats').onclick=()=>cmd('stats');
$('reset').onclick=()=>{if(confirm('Send reset to the PMD2?'))cmd('reset')};
$('info').onclick=async()=>{
  try{
    const d=await j('/api/info');
    $('infotxt').textContent='Name:      '+d.name+'\nVendor ID: 0x'+d.vid.toString(16)+'\nProduct:   0x'+d.pid.toString(16)+
      '\nFirmware:  0x'+d.fw.toString(16)+'\nUID:       '+d.uid+'\nConfig:    v'+d.cfg_ver+' crc 0x'+d.crc.toString(16)+
      '\nAverage:   '+d.avg+'\nOCP scale: '+d.ocp_scale+'\nRotation:  '+d.rotation;
  }catch(e){$('infotxt').textContent='Could not read device info.'}
  $('dlg').showModal();
};

(async()=>{
  try{const h=await j('/api/history');hist.t=h.t;hist.p=h.p;lastSeq=h.seq;draw()}catch(e){draw()}
  poll();
})();
</script></body></html>)HTML";

// ====== Web handlers: dashboard + API ======
void handleDashboard() {
  server.send(200, "text/html", PAGE_DASH);
}

void handleApiSensor() {
  char buf[1600];
  const SensorStruct& s = g_sensor;
  const uint32_t elapsed = (millis() - g_statsStartMs) / 1000;
  const double avg = g_statsSamples ? g_sumPower / g_statsSamples : 0.0;

  int n = snprintf(buf, sizeof(buf),
    "{\"ok\":%d,\"paused\":%d,\"seq\":%lu,\"elapsed\":%lu,"
    "\"vdd\":%u,\"tchip\":%u,\"tsin\":%d,\"tsout\":%d,"
    "\"total\":%u,\"eps\":%u,\"pcie\":%u,\"mb\":%u,"
    "\"peak\":%u,\"avg\":%.1f,\"wh\":%.3f,\"ch\":[",
    (g_sensorValid && g_histCount > 0) ? 1 : 0, g_sampling ? 0 : 1,
    (unsigned long)g_sampleSeq, (unsigned long)elapsed,
    (unsigned)s.Vdd, (unsigned)s.Tchip, (int)s.Ts[0], (int)s.Ts[1],
    (unsigned)s.TotalPower, (unsigned)s.EpsPower, (unsigned)s.PciePower, (unsigned)s.MbPower,
    (unsigned)g_peakPower, avg, g_wattHours);

  for (size_t i = 0; i < SENSOR_POWER_NUM && n > 0 && (size_t)n < sizeof(buf); ++i) {
    const PowerSensor& p = s.PowerReadings[i];
    n += snprintf(buf + n, sizeof(buf) - n, "%s[%u,%lu,%lu,%u]",
                  i ? "," : "",
                  (unsigned)p.Voltage, (unsigned long)p.Current, (unsigned long)p.Power,
                  (unsigned)s.Ocp[i]);
  }
  if (n > 0 && (size_t)n < sizeof(buf)) snprintf(buf + n, sizeof(buf) - n, "]}");

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

void handleApiHistory() {
  String out;
  out.reserve(HIST_LEN * 10 + 64);
  const size_t start = (g_histHead + HIST_LEN - g_histCount) % HIST_LEN;

  out += "{\"seq\":" + String((unsigned long)g_sampleSeq) + ",\"t\":[";
  for (size_t i = 0; i < g_histCount; ++i) {
    if (i) out += ",";
    out += String(g_hist[(start + i) % HIST_LEN].total);
  }
  out += "],\"p\":[";
  for (size_t i = 0; i < g_histCount; ++i) {
    if (i) out += ",";
    out += String(g_hist[(start + i) % HIST_LEN].pcie);
  }
  out += "]}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void handleApiCsv() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Disposition", "attachment; filename=pmd2_log.csv");
  server.send(200, "text/csv", "");
  server.sendContent("age_s,total_w,eps_w,pcie_w,mb_w,ts_in_c,ts_out_c\n");

  const size_t start = (g_histHead + HIST_LEN - g_histCount) % HIST_LEN;
  char line[80];
  for (size_t i = 0; i < g_histCount; ++i) {
    const HistRec& h = g_hist[(start + i) % HIST_LEN];
    snprintf(line, sizeof(line), "%u,%u,%u,%u,%u,%.1f,%.1f\n",
             (unsigned)((g_histCount - 1 - i) * SAMPLE_INTERVAL_MS / 1000),
             (unsigned)h.total, (unsigned)h.eps, (unsigned)h.pcie, (unsigned)h.mb,
             h.tsIn / 10.0f, h.tsOut / 10.0f);
    server.sendContent(line);
  }
  server.sendContent("");   // terminate chunked response
}

void handleApiInfo() {
  uint8_t name[16] = {};
  spiCommandReadRaw(CMD_WELCOME, name, sizeof(name));
  char text[17] = {};
  size_t k = 0;
  for (size_t i = 0; i < sizeof(name); ++i) {
    char c = (char)name[i];
    if (c < 32 || c > 126) break;
    if (c == '"' || c == '\\') c = '?';
    text[k++] = c;
  }

  VendorDataStruct v = {};
  spiCommandReadRaw(CMD_READ_VENDOR_DATA, reinterpret_cast<uint8_t*>(&v), sizeof(v));

  uint8_t uid[12] = {};
  spiCommandReadRaw(CMD_READ_UID, uid, sizeof(uid));
  char uidHex[25] = {};
  for (size_t i = 0; i < sizeof(uid); ++i) snprintf(uidHex + i * 2, 3, "%02X", uid[i]);

  DeviceConfigStruct cfg = {};
  spiCommandReadRaw(CMD_READ_CONFIG, reinterpret_cast<uint8_t*>(&cfg), sizeof(cfg));

  char buf[400];
  snprintf(buf, sizeof(buf),
    "{\"name\":\"%s\",\"vid\":%u,\"pid\":%u,\"fw\":%u,\"uid\":\"%s\","
    "\"cfg_ver\":%u,\"crc\":%u,\"avg\":%ld,\"ocp_scale\":%ld,\"rotation\":%ld}",
    text, (unsigned)v.VendorId, (unsigned)v.ProductId, (unsigned)v.FwVersion, uidHex,
    (unsigned)cfg.Version, (unsigned)cfg.Crc,
    (long)cfg.Average, (long)cfg.OcpScale, (long)cfg.DisplayRotation);

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

void handleApiCmd() {
  const String c = server.arg("c");
  bool ok = true;

  if (c == "pause")           { g_sampling = false; }
  else if (c == "resume")     { g_sampling = true; g_energyPrimed = false; }
  else if (c == "stats")      { resetStats(); }
  else if (c == "reset")      { cmdReset(); }
  else if (c == "mcu_reset")  { cmdMcuCmd(MCU_CMD_RESET); }
  else                        { ok = false; }

  server.send(ok ? 200 : 400, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ====== Web handlers: original text console (now at /console) ======
void handleConsole() {
  String html = R"rawliteral(
  <!DOCTYPE html><html><head><meta charset='utf-8'>
  <title>PMD2 Web Console</title>
  <script>
  async function updateLog() {
  let res = await fetch('/log');
  let text = await res.text();
  document.getElementById('log').textContent = text;
  }
  setInterval(updateLog, 500);
  </script>
  </head><body>
  <h2>PMD2 Web Console</h2>
  <p><a href='/'>&larr; Dashboard</a></p>
  <form action='/cmd' method='POST'>
  Command: <input name='line' type='text'>
  <input type='submit' value='Send'>
  </form>
  <pre id='log'></pre>
  </body></html>
  )rawliteral";
  server.send(200, "text/html", html);
}

void handleCmd() {
  if (server.hasArg("line")) {
    String line = server.arg("line");
    handleCommand(line);
  }
  server.sendHeader("Location", "/console");
  server.send(303);
}

void handleLog() {
  server.send(200, "text/plain", webOutputBuffer);
}

// ====== Setup ======
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1000);

  pinMode(PMD2_CS_PIN, OUTPUT);
  digitalWrite(PMD2_CS_PIN, HIGH);
  SPI.begin();

  dualPrint("PMD2 SPI command tester starting...");
  dualPrint("CS pin: GPIO" + String(PMD2_CS_PIN) + ", SPI mode: 0, clock: " + String(SPI_CLOCK_HZ) + " Hz");

  printHelp();

  g_prefs.begin("pmd2", false);

  // WiFi credentials: prefer what's stored in NVS (set via BLE provisioning
  // or a previous run); fall back to the compiled-in wifi_config.h defaults
  // the first time the device boots.
  String ssid = g_prefs.getString("ssid", WIFI_SSID);
  String pass = g_prefs.getString("pass", WIFI_PASS);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_NAME);
  WiFi.begin(ssid.c_str(), pass.c_str());
  dualPrint("Connecting to WiFi \"" + ssid + "\"...");

  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    dualPrint("WiFi connected, IP: " + WiFi.localIP().toString());
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    dualPrint("WiFi failed. Started AP \"" + String(AP_SSID) + "\", IP: " + WiFi.softAPIP().toString());
  }

  startProvisioningBle();
  dualPrint("BLE provisioning: advertising as \"" + String(BLE_DEVICE_NAME) + "\"");

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    dualPrint("mDNS: http://" + String(MDNS_NAME) + ".local");
  }

  // Web server routes
  server.on("/", HTTP_GET, handleDashboard);
  server.on("/api/sensor", HTTP_GET, handleApiSensor);
  server.on("/api/history", HTTP_GET, handleApiHistory);
  server.on("/api/csv", HTTP_GET, handleApiCsv);
  server.on("/api/info", HTTP_GET, handleApiInfo);
  server.on("/api/cmd", HTTP_POST, handleApiCmd);
  server.on("/console", HTTP_GET, handleConsole);
  server.on("/cmd", HTTP_POST, handleCmd);
  server.on("/log", HTTP_GET, handleLog);
  server.begin();

  resetStats();
}

// ====== Loop ======
void loop() {
  // Serial input
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = g_inputLine;
      g_inputLine = "";
      handleCommand(line);
      continue;
    }
    g_inputLine += c;
  }

  // Dashboard sampling (quiet, always on unless paused from the page)
  if (g_sampling) {
    const uint32_t now = millis();
    if ((now - g_lastSampleMs) >= SAMPLE_INTERVAL_MS) {
      g_lastSampleMs = now;
      sampleTick();
    }
  }

  // Verbose console streaming ("start" command)
  if (g_streamEnabled) {
    const uint32_t now = millis();
    if ((now - g_lastStreamMs) >= STREAM_INTERVAL_MS) {
      g_lastStreamMs = now;
      webOutputBuffer = "";
      sampleOnce();
    }
  }

  // BLE-provisioned WiFi credentials, applied outside the BLE callback
  if (g_bleApplyRequested) {
    applyPendingWifiCredentials();
  }

  // Web server
  server.handleClient();
}
