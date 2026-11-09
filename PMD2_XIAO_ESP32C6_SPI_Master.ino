#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>

// ====== WiFi Config ======
const char* WIFI_SSID = "xxx";
const char* WIFI_PASS = "xxx";

WebServer server(80);
String webInputLine;
String webOutputBuffer;

static constexpr uint32_t SERIAL_BAUD = 115200;
static constexpr uint32_t SPI_CLOCK_HZ = 1000000;
static constexpr uint8_t PMD2_CS_PIN = 17;
static constexpr uint32_t STREAM_INTERVAL_MS = 1000;

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

static_assert(sizeof(VendorDataStruct) == 3, "VendorDataStruct size mismatch");
static_assert(sizeof(SensorStruct) == 192, "SensorStruct size mismatch");
static_assert(sizeof(DeviceConfigStruct) == 159, "DeviceConfigStruct size mismatch");

static bool g_streamEnabled = false;
static String g_inputLine;
static uint32_t g_lastStreamMs = 0;

// ====== Dual output helper ======
void dualPrint(const String& s) {
  Serial.println(s);
  webOutputBuffer += s + "\n";
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

void cmdReadWelcome() {
  uint8_t name[16] = {};
  spiCommandRead(CMD_WELCOME, name, sizeof(name));

  char text[16] = {};
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
  dualPrint("  TsIn=" + String(sensor.Ts[0]/10.0f) + "*C TsOut=" + String(sensor.Ts[1]/10.0f) + "*C");
  dualPrint("  Total=" + String(sensor.TotalPower) + " W");

  dualPrint("Rails:");
  dualPrint("  EPS=" + String(sensor.EpsPower) + " W");
  dualPrint("  PCIE=" + String(sensor.PciePower) + " W");
  dualPrint("  MB=" + String(sensor.MbPower) + " W");

  // Print all power sensor readings
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
  Serial.println("RESET command sent");
}

void cmdMcuCmd(uint8_t mcu_cmd) {
  uint8_t tx_buf[5];
  tx_buf[0] = MCU_CMD_KEY&0xFF;
  tx_buf[1] = MCU_CMD_KEY>>8;
  tx_buf[2] = MCU_CMD_KEY>>16;
  tx_buf[3] = MCU_CMD_KEY>>24;
  tx_buf[4] = mcu_cmd;
  
  spiCommandWrite(CMD_MCU_CMD, tx_buf, 5);
}

void sampleOnce() {
  cmdReadSensor();
}

// ====== Modified command handlers ======
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
  dualPrint("  start     - stream sensor every 250 ms");
  dualPrint("  stop      - stop streaming");
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
  if (line.equalsIgnoreCase("mcu_cmd_reset")) { cmdMcuCmd(0x03); return; }

  dualPrint("Unknown command: " + line);
  printHelp();
}

// ====== Web server handlers ======
void handleRoot() {
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
  server.sendHeader("Location", "/");
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

  // WiFi init
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  dualPrint("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  dualPrint("WiFi connected, IP: " + WiFi.localIP().toString());

  // Web server routes
  server.on("/", handleRoot);
  server.on("/cmd", HTTP_POST, handleCmd);
  server.on("/log", handleLog);
  server.begin();
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

  // Streaming
  if (g_streamEnabled) {
    uint32_t now = millis();
    if ((now - g_lastStreamMs) >= STREAM_INTERVAL_MS) {
      g_lastStreamMs = now;
      webOutputBuffer = "";
      sampleOnce();
    }
  }

  // Web server
  server.handleClient();
}