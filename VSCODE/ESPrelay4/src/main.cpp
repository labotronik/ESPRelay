// main.cpp — ESP32-S3 + W5500 (Ethernet) + PCA9538 @0x70 + LittleFS
// + Règles simples (FOLLOW/AND/OR/XOR/TOGGLE_RISE/PULSE_RISE + delays)
// + Volet (shutter) sur 2 relais AU CHOIX, avec:
//   - réservation des relais (règles simples + override interdits sur ces relais)
//   - interlock UP/DOWN garanti
//   - dead-time entre inversions
//   - max_run_ms (sécurité)
//   - mode hold / toggle
//
// Endpoints:
//   GET  /                 -> index.html depuis LittleFS (streaming)
//   GET  /api/state        -> états inputs/relays/override + info eth + shutter
//   GET  /api/rules        -> JSON des règles (inclut shutters[])
//   GET  /api/net          -> config réseau (dhcp/static)
//   GET  /api/wifi         -> config/status WiFi (AP fallback)
//   PUT  /api/rules        -> remplace les règles (validation volet + conflits)
//   PUT  /api/net          -> applique + sauvegarde config réseau
//   PUT  /api/wifi         -> active/désactive le WiFi
//   POST /api/ota          -> update firmware (application/octet-stream)
//   POST /api/otafs        -> update LittleFS (application/octet-stream)
//   POST /api/override     -> override d'un relais (REFUSE si relais réservé volet)
//   POST /api/shutter      -> commande volet (UP/DOWN/STOP) (seul moyen "API" de bouger les relais volet)


#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <NimBLEDevice.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <cstring>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>

#ifndef RXD0
#define RXD0 44
#endif
#ifndef TXD0
#define TXD0 43
#endif

#ifndef TINY_GSM_MODEM_SIM7600
#define TINY_GSM_MODEM_SIM7600
#endif
#ifndef TINY_GSM_RX_BUFFER
#define TINY_GSM_RX_BUFFER 1024
#endif
#include <TinyGsmClient.h>

// ===================== A ADAPTER A TON PCB =====================
static const uint8_t PIN_LED = 40;
static const uint8_t PIN_ONEWIRE = 1;  // DS18B20 (IO1)
static const uint8_t PIN_DHT = 2;      // DHT22 (IO2)
static const uint8_t PIN_FACTORY = 0;  // IO0 factory reset button (hold 10s at boot)
static const int PIN_MODEM_EN = 3;     // Modem power-enable (PEN)
static const int PIN_MODEM_RX = RXD0;  // ESP RXD0 <- A7670 TX
static const int PIN_MODEM_TX = TXD0;  // ESP TXD0 -> A7670 RX
static const uint32_t MODEM_UART_BAUD = 115200;
static const int MODEM_RX = PIN_MODEM_RX;
static const int MODEM_TX = PIN_MODEM_TX;
// Optional A7670 control pins (-1 = not connected on your PCB)
static const int PIN_MODEM_PWRKEY = 15;  // Active LOW pulse to power on/off
static const int PIN_MODEM_STATUS = -1;  // High when modem is on (if wired)
static const int PIN_MODEM_NET = 16;     // NETLIGHT output from modem
// A7670 HW design guide: PWRKEY low pulse >=50 ms, UART ready ~11.2 s
static const uint32_t MODEM_PWRKEY_ON_PULSE_MS = 80;
static const uint32_t MODEM_BOOT_WAIT_MS = 12000;

// GPRS credentials (used as defaults if MQTT GSM fields are empty)
static const char GPRS_DEFAULT_APN[] = "iot.1nce.net";
static const char GPRS_DEFAULT_USER[] = "";
static const char GPRS_DEFAULT_PASS[] = "";
// SIM card PIN (leave empty if not used)
static const char SIM_PIN[] = "";
// MQTT keepalive in seconds (default 30 min for low GSM traffic)
static const uint16_t MQTT_KEEPALIVE_SECONDS = 1800;
static const uint16_t MQTT_SOCKET_TIMEOUT_SECONDS = 1;
static const uint32_t MQTT_STARTUP_GRACE_MS = 15000;

// I2C (PCA9538)
static const int I2C_SDA = 8;     
static const int I2C_SCL = 9;     
static const uint8_t PCA_BASE_ADDR = 0x70;
static const uint8_t PCA_MAX_MODULES = 4;
static const uint8_t RELAYS_PER_MODULE = 4;
static const uint8_t INPUTS_PER_MODULE = 4;
static const uint8_t MAX_RELAYS = PCA_MAX_MODULES * RELAYS_PER_MODULE;
static const uint8_t MAX_INPUTS = PCA_MAX_MODULES * INPUTS_PER_MODULE;
static const uint8_t SHUTTER_MAX = MAX_RELAYS / 2;
static const uint8_t TEMP_MAX_SENSORS = 8;

// W5500 (SPI)
static const int PIN_W5500_CS = 10;
// ===============================================================

// relais actifs bas ? (si tes relais s'activent quand IO=0)
static const bool RELAY_ACTIVE_LOW = false;

// ===================== PCA9538 REGISTRES =======================
static const uint8_t REG_INPUT  = 0x00;
static const uint8_t REG_OUTPUT = 0x01;
static const uint8_t REG_POL    = 0x02;
static const uint8_t REG_CFG    = 0x03;

// ================== EthernetServer compat ESP32 =================
class EthernetServerCompat final : public EthernetServer {
public:
  explicit EthernetServerCompat(uint16_t port) : EthernetServer(port) {}
  void begin(uint16_t port = 0) override { (void)port; EthernetServer::begin(); }
};

EthernetServerCompat server(80);
static WiFiServer wifiServer(80);
static DNSServer wifiDns;

// BLE (read-only state JSON)
static NimBLEServer* bleServer = nullptr;
static NimBLECharacteristic* bleStateChar = nullptr;
static NimBLECharacteristic* bleStateReadChar = nullptr;
static bool bleClientConnected = false;
static uint32_t bleLastNotifyMs = 0;
static String bleServiceUuid;
static String bleStateCharUuid;
static String bleStateReadCharUuid;
static bool bleInitialized = false;
static bool bleEnabled = true;

static void buildStateJson(String &out);
static void buildStateJsonBle(String &out);
static bool saveBleCfg();
static bool loadBleCfg();
static bool clientWriteAll(Client& c, const uint8_t* data, size_t len, uint32_t timeoutMs = 1500);
static bool clientWriteString(Client& c, const String& s, uint32_t timeoutMs = 1500);

static String macHex12Upper(){
  uint64_t mac64 = ESP.getEfuseMac();
  uint8_t b[6];
  b[0] = (mac64 >> 40) & 0xFF;
  b[1] = (mac64 >> 32) & 0xFF;
  b[2] = (mac64 >> 24) & 0xFF;
  b[3] = (mac64 >> 16) & 0xFF;
  b[4] = (mac64 >> 8) & 0xFF;
  b[5] = (mac64 >> 0) & 0xFF;
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
  return String(buf);
}

static void buildBleUuids(){
  String mac = macHex12Upper();
  bleServiceUuid = "6E400001-B5A3-F393-E0A9-" + mac;
  bleStateCharUuid = "6E400003-B5A3-F393-E0A9-" + mac;
  bleStateReadCharUuid = "6E400004-B5A3-F393-E0A9-" + mac;
}

// ===================== Réseau (à adapter) ======================
byte mac[6] = { 0,0,0,0,0,0 };

struct NetConfig {
  bool dhcp;
  IPAddress ip;
  IPAddress gw;
  IPAddress sn;
  IPAddress dns;
};

struct WifiConfig {
  bool enabled;   // autorise le WiFi AP fallback
  String ssid;
  String pass;
};

static NetConfig netCfg = {
  false,
  IPAddress(192,168,1,50),
  IPAddress(192,168,1,1),
  IPAddress(255,255,255,0),
  IPAddress(192,168,1,1)
};

static const IPAddress WIFI_AP_IP(192,168,4,1);
static const IPAddress WIFI_AP_GW(192,168,4,1);
static const IPAddress WIFI_AP_SN(255,255,255,0);
static const char* WIFI_DEFAULT_PASS = "esprelay4";

static WifiConfig wifiCfg = {
  true,
  String(""),
  String(WIFI_DEFAULT_PASS)
};

static bool wifiApOn = false;
static uint32_t wifiLastCheckMs = 0;

static void buildEthernetMac() {
  uint64_t mac64 = ESP.getEfuseMac();
  mac[0] = (mac64 >> 40) & 0xFF;
  mac[1] = (mac64 >> 32) & 0xFF;
  mac[2] = (mac64 >> 24) & 0xFF;
  mac[3] = (mac64 >> 16) & 0xFF;
  mac[4] = (mac64 >> 8) & 0xFF;
  mac[5] = 0xFE;
}

// ===================== MQTT ======================
struct MqttConfig {
  bool enabled;
  String transport;
  String host;
  uint16_t port;
  String user;
  String pass;
  String gsmMqttHost;
  uint16_t gsmMqttPort;
  String gsmMqttUser;
  String gsmMqttPass;
  String clientId;
  String base;
  String discoveryPrefix;
  bool retain;
  String apn;
  String gsmUser;
  String gsmPass;
};

static MqttConfig mqttCfg = {
  true,                    // enabled: active/desactive MQTT
  String("auto"),          // transport: ethernet | gsm | auto
  String("192.168.1.43"),  // host: broker MQTT principal (Ethernet/auto)
  1883,                    // port: port du broker principal
  String(""),              // user: identifiant MQTT principal
  String(""),              // pass: mot de passe MQTT principal
  String(""),              // gsmMqttHost: broker dedie GSM (optionnel)
  0,                       // gsmMqttPort: port broker dedie GSM
  String(""),              // gsmMqttUser: identifiant MQTT dedie GSM
  String(""),              // gsmMqttPass: mot de passe MQTT dedie GSM
  String("ESPRelay4"),     // clientId: identifiant client MQTT
  String("esprelay4"),     // base: topic racine
  String("homeassistant"), // discoveryPrefix: prefix Home Assistant discovery
  true,                    // retain: publier les etats avec le flag retain
  String(GPRS_DEFAULT_APN),   // apn: APN data pour connexion GSM
  String(GPRS_DEFAULT_USER),  // gsmUser: user APN (GPRS)
  String(GPRS_DEFAULT_PASS)   // gsmPass: mot de passe APN (GPRS)
};

static EthernetClient mqttEth;
static HardwareSerial& SerialAT = Serial1;
static TinyGsm modem(SerialAT);
static TinyGsmClient mqttGsm(modem);
static PubSubClient mqttClientEth(mqttEth);
static PubSubClient mqttClientGsm(mqttGsm);
static uint32_t mqttLastConnectEthMs = 0;
static uint32_t mqttLastConnectGsmMs = 0;
static uint32_t mqttLastEthFailMs = 0;
static int mqttLastEthFailRc = 0;
static bool mqttEthAuthBlocked = false;
static bool mqttStartupGraceLogged = false;
static bool mqttAnnouncedEth = false;
static bool mqttAnnouncedGsm = false;
static bool mqttDisabledWarned = false;
static volatile bool mqttFastCommandPending = false;
static uint32_t mqttFastModeUntilMs = 0;
static bool modemSerialReady = false;
static bool modemReady = false;
static bool modemPowerKickDone = false;
static bool simPinChecked = false;
static bool gsmNetworkReady = false;
static bool gsmDataReady = false;
static uint32_t gsmLastTryMs = 0;
static uint32_t gsmLastDebugMs = 0;
static bool gsmLastNetConnected = false;
static bool gsmLastDataConnected = false;
static int gsmLastCsq = -1;
static int gsmLastDbm = 0;
static String gsmLastOperator = "";
static String gsmLastApn = "";
static String gsmLastIp = "";
static String gsmLastCcid = "";
static int gsmLastNetPin = -1;
static int gsmLastStatusPin = -1;
static uint32_t modemUartBaud = MODEM_UART_BAUD;
static int modemUartRxPin = MODEM_RX;
static int modemUartTxPin = MODEM_TX;
static String lastIpPubEth = "";
static String lastIpPubGsm = "";
static bool lastInputsPub[MAX_INPUTS] = {false};
static bool lastRelaysPub[MAX_RELAYS] = {false};
static int8_t lastRelayModePub[MAX_RELAYS] = {2};
static int lastShutterMove[SHUTTER_MAX] = {-1,-1};

// ===================== 1-Wire (DS18B20) ======================
static OneWire oneWire(PIN_ONEWIRE);
static DallasTemperature tempSensors(&oneWire);
static DeviceAddress tempAddr[TEMP_MAX_SENSORS];
static float tempC[TEMP_MAX_SENSORS];
static float lastTempPub[TEMP_MAX_SENSORS];
static uint8_t tempCount = 0;
static uint32_t lastTempReadMs = 0;

static uint8_t shuttersLimit();

static DHT dht(PIN_DHT, DHT22);
static bool dhtPresent = false;
static float dhtTempC = NAN;
static float lastDhtPub = NAN;
static float dhtHum = NAN;
static float lastDhtHumPub = NAN;
static bool dhtCheckDone = false;
static uint32_t dhtNextProbeMs = 0;


struct AuthConfig {
  String user;
  String pass;
};

static AuthConfig authCfg = {"admin", "admin"};

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#ifndef FW_TAG
#define FW_TAG ""
#endif


// ===================== Etat IO =====================
bool inputs[MAX_INPUTS] = {0};
bool prevInputs[MAX_INPUTS] = {0};
bool rawInputs[MAX_INPUTS] = {0};
uint32_t inputChangeMs[MAX_INPUTS] = {0};
static const uint32_t INPUT_DEBOUNCE_MS = 20;
bool virtualInputs[MAX_INPUTS] = {0};
bool lastVirtualPub[MAX_INPUTS] = {0};
bool combinedInputs[MAX_INPUTS] = {0};
bool prevCombinedInputs[MAX_INPUTS] = {0};

bool relays[MAX_RELAYS] = {0};
bool relayFromSimple[MAX_RELAYS] = {0};
bool relayFromShutter[MAX_RELAYS] = {0};

uint8_t pcaOutCache[PCA_MAX_MODULES] = {0};
bool pcaPresent[PCA_MAX_MODULES] = {false};
bool pcaAlive[PCA_MAX_MODULES] = {false};
uint8_t pcaFailCount[PCA_MAX_MODULES] = {0};
uint32_t pcaLastOkMs[PCA_MAX_MODULES] = {0};
uint8_t pcaCount = 0;
uint8_t totalRelays = 4;
uint8_t totalInputs = 4;

// Overrides : -1 auto, 0 force off, 1 force on (uniquement pour relais NON réservés)
int8_t overrideRelay[MAX_RELAYS];
String lastRulePub[MAX_RELAYS];
bool lastWifiPub = false;
bool lastBlePub = false;

// Mémoire toggle + pulse pour règles simples
bool toggleState[MAX_RELAYS] = {0};
uint32_t pulseUntilMs[MAX_RELAYS] = {0};

// Delay state pour règles simples
bool pendingTarget[MAX_RELAYS] = {0};
bool hasPending[MAX_RELAYS] = {0};
uint32_t pendingDeadlineMs[MAX_RELAYS] = {0};

// Réservation des relais par volet
bool reservedByShutter[MAX_RELAYS] = {false};

// ===================== Règles JSON en RAM ======================
JsonDocument rulesDoc;

// ===============================================================
// I2C helpers (STOP entre write et read => évite i2cWriteReadNonStop)
// ===============================================================
static bool i2cReadReg8(uint8_t addr, uint8_t reg, uint8_t &val) {
  for(int attempt=0; attempt<3; attempt++){
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(true) != 0) { delay(2); continue; } // STOP
    if (Wire.requestFrom((int)addr, 1) != 1) { delay(2); continue; }
    val = Wire.read();
    return true;
  }
  return false;
}

static bool i2cWriteReg8(uint8_t addr, uint8_t reg, uint8_t val) {
  for(int attempt=0; attempt<3; attempt++){
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    if (Wire.endTransmission(true) == 0) return true; // STOP
    delay(2);
  }
  return false;
}

// ===============================================================
// LittleFS helpers
// ===============================================================
static String readFile(const char* path) {
  if (!LittleFS.exists(path)) return "";
  File f = LittleFS.open(path, "r");
  if(!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

static bool writeFile(const char* path, const String& data) {
  File f = LittleFS.open(path, "w");
  if(!f) return false;
  f.print(data);
  f.close();
  return true;
}

// ===============================================================
// WiFi AP helpers (fallback when Ethernet link OFF)
// ===============================================================
static String defaultWifiSsid() {
  uint64_t mac64 = ESP.getEfuseMac();
  uint8_t b2 = (mac64 >> 16) & 0xFF;
  uint8_t b1 = (mac64 >> 8) & 0xFF;
  uint8_t b0 = (mac64 >> 0) & 0xFF;
  char buf[32];
  snprintf(buf, sizeof(buf), "ESPRelay4-%02X%02X%02X", b2, b1, b0);
  return String(buf);
}

static void wifiCfgToJson(String &out) {
  static JsonDocument doc;
  doc.clear();
  doc["enabled"] = wifiCfg.enabled ? 1 : 0;
  doc["ssid"] = wifiCfg.ssid;
  doc["pass"] = wifiCfg.pass;
  doc["ap"] = wifiApOn ? 1 : 0;
  doc["ip"] = wifiApOn ? WiFi.softAPIP().toString() : "";
  doc["eth_link"] = (Ethernet.linkStatus()==LinkON) ? 1 : 0;
  serializeJsonPretty(doc, out);
}

static bool saveWifiCfg(){
  String out;
  wifiCfgToJson(out);
  return writeFile("/wifi.json", out);
}

static bool loadWifiCfg(){
  String s = readFile("/wifi.json");
  if(s.length() == 0){
    wifiCfg.enabled = true;
    wifiCfg.ssid = defaultWifiSsid();
    wifiCfg.pass = String(WIFI_DEFAULT_PASS);
    saveWifiCfg();
    Serial.println("[WIFI] created default /wifi.json");
    return true;
  }
  static JsonDocument doc;
  doc.clear();
  auto err = deserializeJson(doc, s);
  if(err){
    Serial.printf("[WIFI] JSON parse error -> keep default (%s)\n", err.c_str());
    wifiCfg.enabled = true;
    wifiCfg.ssid = defaultWifiSsid();
    wifiCfg.pass = String(WIFI_DEFAULT_PASS);
    return false;
  }
  String defSsid = defaultWifiSsid();
  wifiCfg.enabled = (doc["enabled"] | 1) ? true : false;
  wifiCfg.ssid = String((const char*)(doc["ssid"] | defSsid.c_str()));
  wifiCfg.pass = String((const char*)(doc["pass"] | WIFI_DEFAULT_PASS));
  if(wifiCfg.ssid.length() == 0){
    wifiCfg.ssid = defSsid;
  }
  if(wifiCfg.pass.length() < 8){
    wifiCfg.pass = String(WIFI_DEFAULT_PASS);
  }
  return true;
}

static void startWifiAp(){
  if(wifiApOn) return;
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(WIFI_AP_IP, WIFI_AP_GW, WIFI_AP_SN);
  const bool ok = WiFi.softAP(wifiCfg.ssid.c_str(), wifiCfg.pass.c_str());
  if(ok){
    wifiServer.begin();
    wifiDns.start(53, "*", WiFi.softAPIP());
    wifiApOn = true;
    Serial.printf("[WIFI] AP ON SSID=%s PASS=%s IP=%s\n", wifiCfg.ssid.c_str(), wifiCfg.pass.c_str(), WiFi.softAPIP().toString().c_str());
  } else {
    wifiApOn = false;
    Serial.println("[WIFI] AP start FAILED");
  }
}

static void stopWifiAp(){
  if(!wifiApOn) return;
  wifiDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiApOn = false;
  Serial.println("[WIFI] AP OFF");
}

static void updateWifiState(bool force=false){
  uint32_t now = millis();
  if(!force && (now - wifiLastCheckMs < 1000)) return;
  wifiLastCheckMs = now;
  // Keep AP available when enabled to guarantee local access even if LAN routing is broken.
  const bool shouldRun = wifiCfg.enabled;
  if(shouldRun && !wifiApOn) startWifiAp();
  else if(!shouldRun && wifiApOn) stopWifiAp();
}

static void applyWifiCfg(){
  updateWifiState(true);
}

// BLE config (LittleFS)
static void bleCfgToJson(String &out){
  static JsonDocument doc;
  doc.clear();
  doc["enabled"] = bleEnabled ? 1 : 0;
  serializeJsonPretty(doc, out);
}

static bool saveBleCfg(){
  String out;
  bleCfgToJson(out);
  return writeFile("/ble.json", out);
}

static bool loadBleCfg(){
  String s = readFile("/ble.json");
  if(s.length() == 0){
    bleEnabled = true;
    saveBleCfg();
    Serial.println("[BLE] created default /ble.json");
    return true;
  }
  static JsonDocument doc;
  doc.clear();
  auto err = deserializeJson(doc, s);
  if(err){
    Serial.printf("[BLE] JSON parse error -> keep default (%s)\n", err.c_str());
    bleEnabled = true;
    return false;
  }
  bleEnabled = (doc["enabled"] | 1) ? true : false;
  return true;
}

class BleServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s) override {
    bleClientConnected = true;
  }
  void onDisconnect(NimBLEServer* s) override {
    bleClientConnected = false;
    NimBLEDevice::startAdvertising();
  }
};

static void initBle(){
  if(!bleEnabled) return;
  String name = defaultWifiSsid(); // ESPRelay4-XXXXXX
  buildBleUuids();
  NimBLEDevice::init(name.c_str());
  NimBLEDevice::setMTU(185);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCallbacks());
  NimBLEService* svc = bleServer->createService(bleServiceUuid.c_str());
  bleStateChar = svc->createCharacteristic(
    bleStateCharUuid.c_str(),
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  bleStateReadChar = svc->createCharacteristic(
    bleStateReadCharUuid.c_str(),
    NIMBLE_PROPERTY::READ
  );
  bleStateChar->setValue("{}");
  bleStateReadChar->setValue("{}");
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(bleServiceUuid.c_str());
  adv->setScanResponse(true);
  adv->start();
  bleInitialized = true;
  Serial.printf("[BLE] advertising as %s svc=%s\n", name.c_str(), bleServiceUuid.c_str());
}

static void bleTick(){
  if(!bleEnabled) return;
  if(!bleClientConnected || !bleStateChar) return;
  uint32_t now = millis();
  if(now - bleLastNotifyMs < 1000) return;
  bleLastNotifyMs = now;
  String out;
  buildStateJsonBle(out);
  if(bleStateReadChar){
    bleStateReadChar->setValue((uint8_t*)out.c_str(), out.length());
  }
  // Frame: 0x1E 0x1E + payload + 0x1F
  const uint8_t start[2] = {0x1E, 0x1E};
  const uint8_t end = 0x1F;

  const size_t maxChunk = 160; // safe for BLE notifications
  std::string framed;
  framed.reserve(out.length() + 3);
  framed.append((const char*)start, 2);
  framed.append(out.c_str(), out.length());
  framed.push_back((char)end);

  size_t offset = 0;
  while(offset < framed.size()){
    size_t n = framed.size() - offset;
    if(n > maxChunk) n = maxChunk;
    bleStateChar->setValue((uint8_t*)framed.data() + offset, n);
    bleStateChar->notify();
    offset += n;
  }
}

static void setBleEnabled(bool en){
  if(en == bleEnabled) return;
  bleEnabled = en;
  if(!bleInitialized){
    if(bleEnabled) initBle();
    return;
  }
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if(bleEnabled){
    if(adv) adv->start();
  } else {
    if(adv) adv->stop();
    if(bleClientConnected && bleServer){
      bleServer->disconnect(0);
    }
    bleClientConnected = false;
  }
}

// Auth config (LittleFS)
static void authCfgToJson(String &out){
  static JsonDocument doc;
  doc.clear();
  doc["user"] = authCfg.user;
  doc["pass"] = authCfg.pass;
  serializeJsonPretty(doc, out);
}

static bool saveAuthCfg(){
  String out;
  authCfgToJson(out);
  return writeFile("/auth.json", out);
}

static bool loadAuthCfg(){
  String s = readFile("/auth.json");
  if(s.length() == 0){
    saveAuthCfg();
    Serial.println("[AUTH] created default /auth.json");
    return true;
  }
  static JsonDocument doc;
  doc.clear();
  auto err = deserializeJson(doc, s);
  if(err){
    Serial.printf("[AUTH] JSON parse error -> keep default (%s)\n", err.c_str());
    return false;
  }
  authCfg.user = String((const char*)(doc["user"] | "admin"));
  authCfg.pass = String((const char*)(doc["pass"] | "admin"));
  return true;
}

static bool factoryResetHeld(){
  pinMode(PIN_FACTORY, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, 0);
  if(digitalRead(PIN_FACTORY) != LOW) return false;
  uint32_t start = millis();
  uint32_t lastBlink = start;
  bool ledOn = false;
  while(millis() - start < 10000){
    if(digitalRead(PIN_FACTORY) != LOW) return false;
    uint32_t now = millis();
    if(now - lastBlink >= 125){
      lastBlink = now;
      ledOn = !ledOn;
      digitalWrite(PIN_LED, ledOn ? 1 : 0);
    }
    delay(20);
  }
  digitalWrite(PIN_LED, 1);
  return true;
}

static void heartbeatTick(){
  static uint32_t lastHb = 0;
  static bool hbOn = false;
  uint32_t now = millis();
  if(now - lastHb >= 1000){
    lastHb = now;
    hbOn = !hbOn;
    digitalWrite(PIN_LED, hbOn ? 1 : 0);
  }
}

static void logFactoryPinState(){
  pinMode(PIN_FACTORY, INPUT_PULLUP);
  int v = digitalRead(PIN_FACTORY);
  Serial.printf("[FACTORY] PIN_FACTORY=%d (%s)\n", v, (v==LOW ? "PRESSED" : "RELEASED"));
}

static void doFactoryReset(){
  Serial.println("[FACTORY] button held 10s -> reset config");
  const char* files[] = {"/net.json", "/mqtt.json", "/rules.json", "/auth.json", "/wifi.json", "/ble.json"};
  for(size_t i=0;i<sizeof(files)/sizeof(files[0]);i++){
    if(LittleFS.exists(files[i])){
      LittleFS.remove(files[i]);
      Serial.printf("[FACTORY] removed %s\n", files[i]);
    }
  }
  delay(200);
  ESP.restart();
}

// ===============================================================
// Network config (LittleFS)
// ===============================================================
static bool parseIp(const String& s, IPAddress &out) {
  String t = s;
  t.trim();
  if (t.length() == 0) return false;

  int a[4] = {0,0,0,0};
  int idx = 0;
  int acc = 0;
  int digits = 0;

  for (int i = 0; i <= t.length(); i++) {
    char c = (i == t.length()) ? '.' : t[i];
    if (c == '.') {
      if (digits == 0 || idx > 3) return false;
      if (acc < 0 || acc > 255) return false;
      a[idx++] = acc;
      acc = 0;
      digits = 0;
      continue;
    }
    if (c < '0' || c > '9') return false;
    acc = acc * 10 + (c - '0');
    if (acc > 255) return false;
    digits++;
  }

  if (idx != 4) return false;
  out = IPAddress(a[0], a[1], a[2], a[3]);
  return true;
}

static void netCfgToJson(String &out) {
  static JsonDocument doc;
  doc.clear();
  doc["mode"] = netCfg.dhcp ? "dhcp" : "static";
  if(netCfg.dhcp){
    doc["ip"] = Ethernet.localIP().toString();
    doc["gw"] = Ethernet.gatewayIP().toString();
    doc["sn"] = Ethernet.subnetMask().toString();
    doc["dns"] = Ethernet.dnsServerIP().toString();
  } else {
    doc["ip"] = netCfg.ip.toString();
    doc["gw"] = netCfg.gw.toString();
    doc["sn"] = netCfg.sn.toString();
    doc["dns"] = netCfg.dns.toString();
  }
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  doc["mac"] = macStr;
  serializeJsonPretty(doc, out);
}

static bool saveNetCfg() {
  String out;
  netCfgToJson(out);
  return writeFile("/net.json", out);
}

static bool loadNetCfg() {
  String s = readFile("/net.json");
  if (s.length() == 0) {
    saveNetCfg();
    Serial.println("[NET] created default /net.json");
    return true;
  }
  static JsonDocument doc;
  doc.clear();
  auto err = deserializeJson(doc, s);
  if (err) {
    Serial.printf("[NET] JSON parse error -> keep default (%s)\n", err.c_str());
    return false;
  }
  const char* mode = doc["mode"] | "static";
  netCfg.dhcp = (strcmp(mode, "dhcp") == 0);

  if (!netCfg.dhcp) {
    IPAddress ip, gw, sn, dns;
    bool ok = true;
    const char* ipStr = doc["ip"] | "";
    const char* gwStr = doc["gw"] | "";
    const char* snStr = doc["sn"] | "";
    const char* dnsStr = doc["dns"] | "";
    ok &= parseIp(String(ipStr), ip);
    ok &= parseIp(String(gwStr), gw);
    ok &= parseIp(String(snStr), sn);
    ok &= parseIp(String(dnsStr), dns);
    if (ok) {
      netCfg.ip = ip;
      netCfg.gw = gw;
      netCfg.sn = sn;
      netCfg.dns = dns;
    } else {
      Serial.println("[NET] invalid static IP fields -> keep default");
    }
  }
  Serial.println("[NET] loaded /net.json");
  return true;
}

static void applyNetCfg() {
  Ethernet.init(PIN_W5500_CS);
  if (netCfg.dhcp) {
    Ethernet.begin(mac);
  } else {
    Ethernet.begin(mac, netCfg.ip, netCfg.dns, netCfg.gw, netCfg.sn);
  }
  server.begin();
}

// Streaming file (évite page HTML tronquée)
static void sendFile(Client& client, const char* path, const char* contentType) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    String body = String("File not found: ") + path + "\n";
    String hdr = "HTTP/1.1 404 Not Found\r\n";
    hdr += "Content-Type: text/plain; charset=utf-8\r\n";
    hdr += "Connection: close\r\n";
    hdr += "Content-Length: " + String(body.length()) + "\r\n\r\n";
    clientWriteString(client, hdr, 4000);
    clientWriteString(client, body, 4000);
    return;
  }

  size_t size = f.size();
  String hdr = "HTTP/1.1 200 OK\r\n";
  hdr += "Content-Type: ";
  hdr += contentType;
  hdr += "\r\n";
  hdr += "Content-Length: " + String((unsigned)size) + "\r\n";
  hdr += "Connection: close\r\n\r\n";
  if (!clientWriteString(client, hdr, 4000)) {
    f.close();
    return;
  }

  uint8_t buf[512];
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (!clientWriteAll(client, buf, (size_t)n, 4000)) {
      break;
    }
    delay(0);
  }
  f.close();
  client.flush();
  delay(5);
}

// ===============================================================
// PCA9538
// ===============================================================
static bool pcaInitModule(uint8_t addr, uint8_t &outCache) {
  Wire.beginTransmission(addr);
  if (Wire.endTransmission(true) != 0) return false;

  // Invert inputs IO4..IO7 (pull-up + active-low buttons)
  if (!i2cWriteReg8(addr, REG_POL, 0xF0)) return false;
  if (!i2cWriteReg8(addr, REG_CFG, 0xF0)) return false; // IO0..3 out, IO4..7 in

  // outputs off
  uint8_t outNibble = RELAY_ACTIVE_LOW ? 0x0F : 0x00;
  outCache = (outCache & 0xF0) | (outNibble & 0x0F);
  if (!i2cWriteReg8(addr, REG_OUTPUT, outCache)) return false;

  return true;
}

static void pcaScanAndInit() {
  pcaCount = 0;
  int lastPresent = -1;
  for (uint8_t m = 0; m < PCA_MAX_MODULES; m++) {
    uint8_t addr = PCA_BASE_ADDR + m;
    pcaPresent[m] = false;
    pcaAlive[m] = false;
    pcaFailCount[m] = 0;
    pcaLastOkMs[m] = 0;
    if (pcaInitModule(addr, pcaOutCache[m])) {
      pcaPresent[m] = true;
      pcaAlive[m] = true;
      pcaLastOkMs[m] = millis();
      if ((int)m > lastPresent) lastPresent = m;
    }
  }
  if (lastPresent >= 0) {
    pcaCount = (uint8_t)(lastPresent + 1);
  } else {
    pcaCount = 1; // fallback logique
  }
  totalRelays = pcaCount * RELAYS_PER_MODULE;
  totalInputs = pcaCount * INPUTS_PER_MODULE;

  for (int i = 0; i < MAX_RELAYS; i++) {
    overrideRelay[i] = -1;
    lastRelayModePub[i] = 2; // force first publish
  }
}

static void pcaReadInputs() {
  for (uint8_t m = 0; m < PCA_MAX_MODULES; m++) {
    uint8_t base = m * INPUTS_PER_MODULE;
    uint8_t in = 0;
    if (!pcaPresent[m]) {
      // try to recover: probe read even if not marked present
      if(!i2cReadReg8(PCA_BASE_ADDR + m, REG_INPUT, in)){
        pcaFailCount[m] = (pcaFailCount[m] < 255) ? (uint8_t)(pcaFailCount[m] + 1) : 255;
        if(pcaFailCount[m] >= 3) pcaAlive[m] = false;
        continue;
      }
      pcaPresent[m] = true;
    } else if(!i2cReadReg8(PCA_BASE_ADDR + m, REG_INPUT, in)){
      pcaFailCount[m] = (pcaFailCount[m] < 255) ? (uint8_t)(pcaFailCount[m] + 1) : 255;
      if(pcaFailCount[m] >= 3) pcaAlive[m] = false;
      continue;
    }
    pcaFailCount[m] = 0;
    pcaAlive[m] = true;
    pcaLastOkMs[m] = millis();
    for(uint8_t i=0;i<INPUTS_PER_MODULE;i++){
      rawInputs[base + i] = (in >> (4+i)) & 0x1; // IO4..IO7
    }
  }
}

static void debounceInputs() {
  const uint32_t now = millis();
  for (int i = 0; i < totalInputs; i++) {
    if (rawInputs[i] != inputs[i]) {
      if (inputChangeMs[i] == 0) inputChangeMs[i] = now;
      if (now - inputChangeMs[i] >= INPUT_DEBOUNCE_MS) {
        inputs[i] = rawInputs[i];
        inputChangeMs[i] = 0;
      }
    } else {
      inputChangeMs[i] = 0;
    }
  }
}
static void pcaApplyRelays() {
  for (uint8_t m = 0; m < PCA_MAX_MODULES; m++) {
    if (!pcaPresent[m]) continue;
    uint8_t base = m * RELAYS_PER_MODULE;
    uint8_t nibble = 0;
    for(uint8_t i=0;i<RELAYS_PER_MODULE;i++){
      bool v = relays[base + i];
      if(RELAY_ACTIVE_LOW) v = !v;
      if(v) nibble |= (1u << i);
    }
    pcaOutCache[m] = (pcaOutCache[m] & 0xF0) | (nibble & 0x0F);
    i2cWriteReg8(PCA_BASE_ADDR + m, REG_OUTPUT, pcaOutCache[m]);
  }
}

// ===============================================================
// Rules defaults + load/save
// ===============================================================
static void addDefaultRelay(JsonArray rel, int i){
  JsonObject r = rel.add<JsonObject>();
  JsonObject expr = r["expr"].to<JsonObject>();
  expr["op"] = "NONE";
  r["invert"] = false;
  r["onDelay"] = 0;
  r["offDelay"] = 0;
  r["pulseMs"] = 200;
}

static void setDefaultRules() {
  rulesDoc.clear();
  rulesDoc["version"] = 2;

  JsonArray rel = rulesDoc["relays"].to<JsonArray>();
  for(int i=0;i<totalRelays;i++) addDefaultRelay(rel, i);

  rulesDoc["shutters"].to<JsonArray>(); // vide par défaut
}

static bool saveRulesToFS(const JsonDocument& doc) {
  String out;
  serializeJsonPretty(doc, out);
  return writeFile("/rules.json", out);
}

static bool loadRulesFromFS() {
  String s = readFile("/rules.json");
  if(s.length() == 0){
    setDefaultRules();
    saveRulesToFS(rulesDoc);
    Serial.println("[RULES] created default /rules.json");
    return true;
  }

  rulesDoc.clear();
  auto err = deserializeJson(rulesDoc, s);
  if(err){
    Serial.printf("[RULES] JSON parse error -> default (%s)\n", err.c_str());
    setDefaultRules();
    saveRulesToFS(rulesDoc);
    return false;
  }

  // ensure base fields
  if(!rulesDoc["relays"].is<JsonArray>() || rulesDoc["relays"].as<JsonArray>().size() != totalRelays){
    Serial.println("[RULES] relays[] size mismatch -> normalize");
    JsonDocument newDoc;
    newDoc["version"] = rulesDoc["version"] | 2;

    JsonArray newRel = newDoc["relays"].to<JsonArray>();
    for(int i=0;i<totalRelays;i++) addDefaultRelay(newRel, i);

    JsonArray oldRel = rulesDoc["relays"].as<JsonArray>();
    int copyCount = (oldRel.size() < totalRelays) ? (int)oldRel.size() : (int)totalRelays;
    for(int i=0;i<copyCount;i++){
      newRel[i].set(oldRel[i]);
    }

    JsonArray shNew = newDoc["shutters"].to<JsonArray>();
    if(rulesDoc["shutters"].is<JsonArray>()){
      for(JsonVariant v : rulesDoc["shutters"].as<JsonArray>()) shNew.add(v);
    }

    rulesDoc.clear();
    rulesDoc.set(newDoc);
    saveRulesToFS(rulesDoc);
    return true;
  }
  if(!rulesDoc["shutters"].is<JsonArray>()){
    rulesDoc.remove("shutters");
    rulesDoc["shutters"].to<JsonArray>();
  }
  if(!rulesDoc["version"].is<int>()) rulesDoc["version"] = 2;

  Serial.println("[RULES] loaded /rules.json");
  return true;
}

// ===============================================================
// Volet (Shutter) — logique + sécurité (réservation)
// ===============================================================
enum ShutterMove : uint8_t { SH_STOP=0, SH_UP=1, SH_DOWN=2 };
enum ManualCmd : uint8_t { MC_NONE=0, MC_UP=1, MC_DOWN=2, MC_STOP=3 };

struct ShutterCfg {
  bool enabled = false;
  String name;

  uint8_t up_in = 1;      // 1..4
  uint8_t down_in = 2;    // 1..4
  uint8_t up_relay = 1;   // 1..4
  uint8_t down_relay = 2; // 1..4

  String mode = "hold";     // "hold" or "toggle"
  String priority = "stop"; // "stop" | "up" | "down"

  uint32_t deadtime_ms = 400;
  uint32_t max_run_ms = 25000; // 0=disabled
};

struct ShutterRuntime {
  ShutterMove move = SH_STOP;
  uint32_t moveStartMs = 0;
  uint32_t cooldownUntilMs = 0;

  // toggle mode memory
  bool lastUpBtn = false;
  bool lastDownBtn = false;

  // API manual command
  ManualCmd manual = MC_NONE;
};

ShutterCfg shCfg[SHUTTER_MAX];
ShutterRuntime shRt[SHUTTER_MAX];

// ===============================================================
// MQTT config (LittleFS)
// ===============================================================
static String normalizeBaseTopic(const String& in) {
  String t = in;
  t.trim();
  if (t.endsWith("/")) t.remove(t.length()-1);
  if (t.length() == 0) t = "esprelay4";
  return t;
}

static String normalizeMqttTransport(const String& in) {
  String t = in;
  t.trim();
  t.toLowerCase();
  if (t == "gsm" || t == "auto") return t;
  return "auto";
}

static String mqttDesiredTransport() {
  return normalizeMqttTransport(mqttCfg.transport);
}

static const uint32_t MQTT_ETH_RETRY_AFTER_FAIL_MS = 30000;
static const uint32_t MQTT_ETH_RETRY_AFTER_AUTH_FAIL_MS = 300000;

static bool mqttShouldTryEthernetNow() {
  if (Ethernet.linkStatus() != LinkON) return false;
  if (mqttLastEthFailMs == 0) return true;
  const uint32_t age = millis() - mqttLastEthFailMs;
  if (mqttEthAuthBlocked) return age >= MQTT_ETH_RETRY_AFTER_AUTH_FAIL_MS;
  return age >= MQTT_ETH_RETRY_AFTER_FAIL_MS;
}

static bool mqttHasDedicatedGsmBroker();

static bool mqttHasDedicatedGsmBroker() {
  return mqttCfg.gsmMqttHost.length() > 0;
}

static bool mqttTransportAllowsEthernet() {
  return mqttDesiredTransport() != "gsm";
}

static bool mqttTransportAllowsGsm() {
  String desired = mqttDesiredTransport();
  if (desired == "gsm") return true;
  // Le mode double client ETH+GSM est active en mode auto avec broker GSM dedie.
  if (desired == "auto") return mqttHasDedicatedGsmBroker();
  return false;
}

static const char* mqttHostForTransport(const String& transport) {
  if (transport == "gsm" && mqttCfg.gsmMqttHost.length() > 0) {
    return mqttCfg.gsmMqttHost.c_str();
  }
  return mqttCfg.host.c_str();
}

static uint16_t mqttPortForTransport(const String& transport) {
  if (transport == "gsm" && mqttCfg.gsmMqttPort > 0) {
    return mqttCfg.gsmMqttPort;
  }
  return mqttCfg.port;
}

static String mqttUserForTransport(const String& transport) {
  if (transport == "gsm" && mqttCfg.gsmMqttUser.length() > 0) {
    return mqttCfg.gsmMqttUser;
  }
  return mqttCfg.user;
}

static String mqttPassForTransport(const String& transport) {
  if (transport == "gsm" && mqttCfg.gsmMqttUser.length() > 0) {
    return mqttCfg.gsmMqttPass;
  }
  return mqttCfg.pass;
}

static bool mqttEthConnectedSafe() {
  return mqttClientEth.connected();
}

static bool mqttCanQueryGsmClient() {
  if (!mqttTransportAllowsGsm()) return false;
  if (!modemSerialReady) return false;
  if (!modemReady) return false;
  return true;
}

static bool mqttGsmConnectedSafe() {
  if (!mqttCanQueryGsmClient()) return false;
  // Prevent stale PubSub state from being reported as connected when GSM data is down.
  if (!gsmNetworkReady || !gsmDataReady) return false;
  return mqttClientGsm.connected();
}

static PubSubClient* mqttClientForTransport(const String& transport) {
  return (transport == "gsm") ? &mqttClientGsm : &mqttClientEth;
}

static bool mqttConnectedForTransport(const String& transport) {
  if (transport == "gsm") return mqttGsmConnectedSafe();
  return mqttEthConnectedSafe();
}

static void mqttApplyServerForTransport(const String& transport) {
  mqttClientForTransport(transport)->setServer(mqttHostForTransport(transport), mqttPortForTransport(transport));
}

static String mqttActiveTransportText() {
  const bool eth = mqttEthConnectedSafe();
  const bool gsm = mqttGsmConnectedSafe();
  if (eth && gsm) return "ethernet+gsm";
  if (eth) return "ethernet";
  if (gsm) return "gsm";
  return "none";
}

static void gsmMarkDown() {
  gsmNetworkReady = false;
  gsmDataReady = false;
  gsmLastNetConnected = false;
  gsmLastDataConnected = false;
}

static bool modemStatusIsOn();
static bool gsmEnsureData();

static void modemDriveExpectedPins() {
  if (PIN_MODEM_EN >= 0) {
    pinMode(PIN_MODEM_EN, OUTPUT);
    digitalWrite(PIN_MODEM_EN, HIGH);
  }
  if (PIN_MODEM_PWRKEY >= 0) {
    pinMode(PIN_MODEM_PWRKEY, OUTPUT);
    digitalWrite(PIN_MODEM_PWRKEY, HIGH); // idle level for active-low PWRKEY
  }
}

static void modemVerifyStartupPins() {
  int pen = -1;
  int pwk = -1;
  if (PIN_MODEM_EN >= 0) pen = digitalRead(PIN_MODEM_EN);
  if (PIN_MODEM_PWRKEY >= 0) pwk = digitalRead(PIN_MODEM_PWRKEY);
  Serial.printf("[MODEM] boot pins PEN=%d (expect 1) PWK=%d (expect 1=idle)\n", pen, pwk);
  if (pen >= 0 && pen != HIGH) {
    Serial.println("[MODEM] WARN PEN is not HIGH at boot");
    digitalWrite(PIN_MODEM_EN, HIGH);
    delay(1);
    Serial.printf("[MODEM] PEN re-drive -> %d\n", digitalRead(PIN_MODEM_EN));
  }
  if (pwk >= 0 && pwk != HIGH) {
    Serial.println("[MODEM] WARN PWK is LOW at boot (could trigger press)");
    digitalWrite(PIN_MODEM_PWRKEY, HIGH);
    delay(1);
    Serial.printf("[MODEM] PWK re-drive -> %d\n", digitalRead(PIN_MODEM_PWRKEY));
  }
}

static String gsmEffectiveApn() {
  String apn = mqttCfg.apn;
  apn.trim();
  if (apn.length() == 0) apn = String(GPRS_DEFAULT_APN);
  return apn;
}

static String gsmEffectiveUser() {
  String u = mqttCfg.gsmUser;
  u.trim();
  if (u.length() == 0) u = String(GPRS_DEFAULT_USER);
  return u;
}

static String gsmEffectivePass() {
  String p = mqttCfg.gsmPass;
  p.trim();
  if (p.length() == 0) p = String(GPRS_DEFAULT_PASS);
  return p;
}

static int gsmRssiToDbm(int csq) {
  if (csq < 0 || csq > 31) return 0;
  return -113 + (2 * csq);
}

static String modemIpToString() {
  IPAddress ip = modem.localIP();
  String out = ip.toString();
  out.trim();
  return out;
}

static void gsmDebug1nce(const char* reason, bool force = false) {
  const uint32_t now = millis();
  if (!force && (now - gsmLastDebugMs < 10000)) return;
  gsmLastDebugMs = now;

  int netPin = -1;
  int statusPin = -1;
  if (PIN_MODEM_NET >= 0) netPin = digitalRead(PIN_MODEM_NET);
  if (PIN_MODEM_STATUS >= 0) statusPin = modemStatusIsOn() ? 1 : 0;
  gsmLastNetPin = netPin;
  gsmLastStatusPin = statusPin;
  gsmLastApn = gsmEffectiveApn();

  if (!modemReady) {
    gsmLastNetConnected = false;
    gsmLastDataConnected = false;
    gsmLastCsq = -1;
    gsmLastDbm = 0;
    gsmLastOperator = "";
    gsmLastIp = "";
    Serial.printf("[GSM][1NCE] %s modem_ready=0 net_pin=%d status_pin=%d sim_pin=%s\n",
                  reason, netPin, statusPin, (strlen(SIM_PIN) > 0 ? "set" : "empty"));
    return;
  }

  const bool net = modem.isNetworkConnected();
  const bool data = modem.isGprsConnected();
  const int csq = modem.getSignalQuality();
  const int dbm = gsmRssiToDbm(csq);
  String op = modem.getOperator();
  op.trim();
  String ip = modemIpToString();
  String apn = gsmLastApn;
  gsmLastNetConnected = net;
  gsmLastDataConnected = data;
  gsmLastCsq = csq;
  gsmLastDbm = dbm;
  gsmLastOperator = op;
  gsmLastIp = ip;

  Serial.printf("[GSM][1NCE] %s net=%d data=%d csq=%d dbm=%d op=%s apn=%s ip=%s net_pin=%d status_pin=%d\n",
                reason,
                net ? 1 : 0,
                data ? 1 : 0,
                csq,
                dbm,
                op.length() ? op.c_str() : "-",
                apn.c_str(),
                ip.length() ? ip.c_str() : "-",
                netPin,
                statusPin);
}

static bool modemStatusIsOn() {
  if (PIN_MODEM_STATUS < 0) return false;
  return digitalRead(PIN_MODEM_STATUS) == HIGH;
}

static void modemPowerKick() {
  if (PIN_MODEM_PWRKEY < 0) return;
  // Keep line released high by default, then send active-low pulse.
  modemDriveExpectedPins();
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);
  delay(MODEM_PWRKEY_ON_PULSE_MS);
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);
  Serial.printf("[GSM] PWRKEY pulse %lums\n", (unsigned long)MODEM_PWRKEY_ON_PULSE_MS);
}

static bool modemProbeAtBaud(uint32_t baud, int rxPin, int txPin, String &rxDump) {
  rxDump = "";
  SerialAT.end();
  SerialAT.begin(baud, SERIAL_8N1, rxPin, txPin);
  delay(80);
  while (SerialAT.available()) SerialAT.read();

  SerialAT.print("AT\r\n");
  const uint32_t t0 = millis();
  while (millis() - t0 < 800) {
    while (SerialAT.available()) {
      char c = (char)SerialAT.read();
      if (rxDump.length() < 180) rxDump += c;
    }
    if (rxDump.indexOf("OK") >= 0) return true;
    delay(2);
  }
  return false;
}

static bool modemProbeAutoBaud() {
  const uint32_t bauds[] = {115200, 9600, 57600, 38400, 19200};
  const int candidates[2][2] = {
    {MODEM_RX, MODEM_TX},
    {MODEM_TX, MODEM_RX}
  };

  for (size_t c = 0; c < 2; c++) {
    const int rxPin = candidates[c][0];
    const int txPin = candidates[c][1];
    for (size_t i = 0; i < (sizeof(bauds) / sizeof(bauds[0])); i++) {
      String rx;
      if (modemProbeAtBaud(bauds[i], rxPin, txPin, rx)) {
        modemUartBaud = bauds[i];
        modemUartRxPin = rxPin;
        modemUartTxPin = txPin;
        modemSerialReady = true;
        Serial.printf("[GSM] AT detected at %lu bps (RX=%d TX=%d)\n",
                      (unsigned long)bauds[i], rxPin, txPin);
        return true;
      }
      if (rx.length() > 0) {
        rx.replace("\r", "\\r");
        rx.replace("\n", "\\n");
        Serial.printf("[GSM] probe %lu bps (RX=%d TX=%d) rx=%s\n",
                      (unsigned long)bauds[i], rxPin, txPin, rx.c_str());
      }
    }
  }
  return false;
}

static bool gsmEnsureData() {
  if (gsmDataReady && modem.isGprsConnected()) return true;
  const uint32_t now = millis();
  if (gsmLastTryMs != 0 && (now - gsmLastTryMs < 10000)) return false;
  gsmLastTryMs = now;
  gsmDebug1nce("check", false);

  modemDriveExpectedPins();

  if (!modemSerialReady) {
    Serial.println("[GSM] wait");
    // Set GSM module baud rate and UART pins
    SerialAT.begin(modemUartBaud, SERIAL_8N1, modemUartRxPin, modemUartTxPin);
    modemSerialReady = true;
    delay(500);
  }

  if (!modemReady) {
    if (!modem.testAT()) {
      if (!modemPowerKickDone && PIN_MODEM_PWRKEY >= 0) {
        modemPowerKick();
        modemPowerKickDone = true;
        delay(MODEM_BOOT_WAIT_MS);
      }
      if (PIN_MODEM_STATUS >= 0) {
        Serial.printf("[GSM] STATUS pin=%d\n", modemStatusIsOn() ? 1 : 0);
      }
  
      if (!modem.testAT()) {
        Serial.println("[GSM] no AT response");
        gsmMarkDown();
        gsmDebug1nce("no_at", true);
        return false;
      }
    }

    // Restart takes quite some time; init() is faster.
    Serial.println("[GSM] Initializing modem...");
    if (!modem.init()) {
      Serial.println("[GSM] modem init failed");
      gsmMarkDown();
      gsmDebug1nce("modem_init_fail", true);
      return false;
    }
    String modemInfo = modem.getModemName();
    modemInfo.trim();
    Serial.printf("[GSM] Modem Name: %s\n", modemInfo.length() ? modemInfo.c_str() : "-");

    modemPowerKickDone = false;
    modemReady = true;
    Serial.println("[GSM] modem ready");
  }

  if (!simPinChecked) {
    simPinChecked = true;
    int simStatus = modem.getSimStatus();
    Serial.printf("[GSM] SIM status=%d\n", simStatus);

    // Unlock SIM card with a PIN if needed
    if (strlen(SIM_PIN) > 0 && simStatus != 3) {
      Serial.println("[GSM] Unlocking sim card...");
      if (!modem.simUnlock(SIM_PIN)) {
        Serial.println("[GSM] SIM unlock failed");
        gsmMarkDown();
        gsmDebug1nce("sim_unlock_fail", true);
        return false;
      }
      delay(300);
      simStatus = modem.getSimStatus();
      Serial.printf("[GSM] SIM status=%d\n", simStatus);
    }

    String ccid = modem.getSimCCID();
    ccid.trim();
    gsmLastCcid = ccid;
    Serial.printf("[GSM] SIM CCID=%s\n", ccid.length() ? ccid.c_str() : "-");

    if (simStatus != 3) {
      Serial.println("[GSM] SIM not ready");
    }
  }

  if (!modem.isNetworkConnected()) {
    if (!modem.waitForNetwork(10000)) {
      Serial.println("[GSM] network not ready");
      gsmMarkDown();
      gsmDebug1nce("network_wait_fail", true);
      return false;
    }
  }

  gsmNetworkReady = modem.isNetworkConnected();
  if (!gsmNetworkReady) {
    Serial.println("[GSM] network attach failed");
    gsmDataReady = false;
    gsmDebug1nce("network_attach_fail", true);
    return false;
  }

  String apn = gsmEffectiveApn();
  String gprsUser = gsmEffectiveUser();
  String gprsPass = gsmEffectivePass();
  if (!modem.isGprsConnected()) {
    if (!modem.gprsConnect(apn.c_str(), gprsUser.c_str(), gprsPass.c_str())) {
      Serial.println("[GSM] gprs connect failed");
      gsmDataReady = false;
      gsmDebug1nce("gprs_fail", true);
      return false;
    }
  }

  gsmDataReady = modem.isGprsConnected();
  if (gsmDataReady) {
    String ip = modemIpToString();
    Serial.printf("[GSM] data connected ip=%s\n", ip.c_str());
    gsmDebug1nce("connect_ok", true);
  }
  return gsmDataReady;
}

static String mqttCurrentIpForTransport(const String& transport) {
  if (transport == "gsm") {
    if (!mqttTransportAllowsGsm()) return "";
    if (!modemSerialReady || !modemReady || !gsmDataReady) return "";
    return modemIpToString();
  }
  return Ethernet.localIP().toString();
}

static String mqttCurrentIp() {
  if (mqttEthConnectedSafe()) return mqttCurrentIpForTransport("ethernet");
  if (mqttGsmConnectedSafe()) return mqttCurrentIpForTransport("gsm");
  return mqttCurrentIpForTransport("ethernet");
}

static void mqttCfgToJson(String &out) {
  static JsonDocument doc;
  doc.clear();
  doc["enabled"] = mqttCfg.enabled ? 1 : 0;
  doc["transport"] = normalizeMqttTransport(mqttCfg.transport);
  doc["host"] = mqttCfg.host;
  doc["port"] = mqttCfg.port;
  doc["user"] = mqttCfg.user;
  doc["pass"] = mqttCfg.pass;
  doc["gsm_mqtt_host"] = mqttCfg.gsmMqttHost;
  doc["gsm_mqtt_port"] = mqttCfg.gsmMqttPort;
  doc["gsm_mqtt_user"] = mqttCfg.gsmMqttUser;
  doc["gsm_mqtt_pass"] = mqttCfg.gsmMqttPass;
  doc["client_id"] = mqttCfg.clientId;
  doc["base"] = mqttCfg.base;
  doc["discovery_prefix"] = mqttCfg.discoveryPrefix;
  doc["retain"] = mqttCfg.retain ? 1 : 0;
  doc["apn"] = mqttCfg.apn;
  doc["gsm_user"] = mqttCfg.gsmUser;
  doc["gsm_pass"] = mqttCfg.gsmPass;
  const bool ethConn = mqttEthConnectedSafe();
  const bool gsmConn = mqttGsmConnectedSafe();
  doc["connected"] = (ethConn || gsmConn) ? 1 : 0;
  doc["eth_connected"] = ethConn ? 1 : 0;
  doc["gsm_connected"] = gsmConn ? 1 : 0;
  doc["active_transport"] = mqttActiveTransportText();
  doc["active_host"] = mqttCfg.host;
  doc["active_port"] = mqttCfg.port;
  doc["eth_host"] = mqttCfg.host;
  doc["eth_port"] = mqttCfg.port;
  doc["gsm_host"] = mqttHostForTransport("gsm");
  doc["gsm_port"] = mqttPortForTransport("gsm");
  doc["eth_ip"] = mqttCurrentIpForTransport("ethernet");
  doc["gsm_ip"] = mqttCurrentIpForTransport("gsm");
  doc["gsm_network"] = gsmNetworkReady ? 1 : 0;
  doc["gsm_data"] = gsmDataReady ? 1 : 0;
  serializeJsonPretty(doc, out);
}

static bool saveMqttCfg() {
  String out;
  mqttCfgToJson(out);
  return writeFile("/mqtt.json", out);
}

static bool loadMqttCfg() {
  String s = readFile("/mqtt.json");
  if (s.length() == 0) {
    if (saveMqttCfg()) {
      Serial.println("[MQTT] created default /mqtt.json");
      return true;
    }
    Serial.println("[MQTT] failed to create default /mqtt.json");
    return false;
  }
  static JsonDocument doc;
  doc.clear();
  auto err = deserializeJson(doc, s);
  if (err) {
    Serial.printf("[MQTT] JSON parse error -> keep default (%s)\n", err.c_str());
    return false;
  }
  mqttCfg.enabled = (doc["enabled"] | 0) ? true : false;
  mqttCfg.transport = normalizeMqttTransport(String((const char*)(doc["transport"] | "auto")));
  mqttCfg.host = String((const char*)(doc["host"] | "192.168.1.43"));
  mqttCfg.port = (uint16_t)(doc["port"] | 1883);
  mqttCfg.user = String((const char*)(doc["user"] | ""));
  mqttCfg.pass = String((const char*)(doc["pass"] | ""));
  if (!doc["gsm_mqtt_host"].isNull()) {
    String loadedGsmHost = String((const char*)(doc["gsm_mqtt_host"] | ""));
    loadedGsmHost.trim();
    if (loadedGsmHost.length() > 0 || mqttCfg.gsmMqttHost.length() == 0) {
      mqttCfg.gsmMqttHost = loadedGsmHost;
    }
  }
  if (!doc["gsm_mqtt_port"].isNull()) {
    mqttCfg.gsmMqttPort = (uint16_t)(doc["gsm_mqtt_port"] | 0);
  }
  if (!doc["gsm_mqtt_user"].isNull()) {
    mqttCfg.gsmMqttUser = String((const char*)(doc["gsm_mqtt_user"] | ""));
  }
  if (!doc["gsm_mqtt_pass"].isNull()) {
    mqttCfg.gsmMqttPass = String((const char*)(doc["gsm_mqtt_pass"] | ""));
  }
  mqttCfg.clientId = String((const char*)(doc["client_id"] | "ESPRelay4"));
  mqttCfg.base = normalizeBaseTopic(String((const char*)(doc["base"] | "esprelay4")));
  mqttCfg.discoveryPrefix = String((const char*)(doc["discovery_prefix"] | "homeassistant"));
  mqttCfg.retain = (doc["retain"] | 1) ? true : false;
  mqttCfg.apn = String((const char*)(doc["apn"] | GPRS_DEFAULT_APN));
  mqttCfg.gsmUser = String((const char*)(doc["gsm_user"] | GPRS_DEFAULT_USER));
  mqttCfg.gsmPass = String((const char*)(doc["gsm_pass"] | GPRS_DEFAULT_PASS));
  mqttCfg.host.trim();
  mqttCfg.gsmMqttHost.trim();
  mqttCfg.apn.trim();
  if (mqttCfg.gsmMqttHost.length() > 0 && mqttCfg.gsmMqttPort == 0) {
    mqttCfg.gsmMqttPort = 1883;
  }
  return true;
}

static void mqttPublishToClient(PubSubClient &client, const String& topic, const String& payload, bool retain) {
  if (&client == &mqttClientGsm) {
    if (!mqttGsmConnectedSafe()) return;
  } else {
    if (!mqttEthConnectedSafe()) return;
  }
  client.publish(topic.c_str(), payload.c_str(), retain);
}

static void mqttPublishToTransport(const String& transport, const String& topic, const String& payload, bool retain) {
  mqttPublishToClient(*mqttClientForTransport(transport), topic, payload, retain);
}

static void mqttPublish(const String& topic, const String& payload, bool retain) {
  mqttPublishToClient(mqttClientEth, topic, payload, retain);
  mqttPublishToClient(mqttClientGsm, topic, payload, retain);
}

static void mqttPublishEthernetOnly(const String& topic, const String& payload, bool retain) {
  mqttPublishToClient(mqttClientEth, topic, payload, retain);
}

static const char* relayModeText(int8_t mode) {
  if (mode == -1) return "AUTO";
  if (mode == 1) return "FORCE_ON";
  return "FORCE_OFF";
}

static bool mqttLowDataTransport(const String& transport) {
  return transport == "gsm";
}

static const char* mqttStateText(int rc) {
  switch (rc) {
    case -4: return "connection timeout";
    case -3: return "connection lost";
    case -2: return "connect failed";
    case -1: return "disconnected";
    case 0:  return "connected";
    case 1:  return "bad protocol";
    case 2:  return "bad client id";
    case 3:  return "server unavailable";
    case 4:  return "bad credentials";
    case 5:  return "unauthorized";
    default: return "unknown";
  }
}

static bool mqttHostLooksPrivateForGsm(const String& host) {
  if (host.startsWith("192.168.")) return true;
  if (host.startsWith("10.")) return true;
  if (host.startsWith("172.")) {
    int d1 = host.indexOf('.');
    int d2 = (d1 >= 0) ? host.indexOf('.', d1 + 1) : -1;
    if (d1 >= 0 && d2 > d1) {
      int second = host.substring(d1 + 1, d2).toInt();
      if (second >= 16 && second <= 31) return true;
    }
  }
  if (host.startsWith("127.")) return true;
  if (host == "localhost") return true;
  return false;
}

static String mqttBaseTopic() {
  return normalizeBaseTopic(mqttCfg.base);
}

static String mqttNodeId() {
  String id = mqttCfg.clientId;
  id.trim();
  if (id.length() == 0) id = "ESPRelay4";
  return id;
}

static String tempAddrToString(const DeviceAddress &a){
  char buf[17];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X%02X%02X",
           a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
  return String(buf);
}

static String ruleSummaryShort(int relayIndex){
  JsonArray rel = rulesDoc["relays"].as<JsonArray>();
  if(!rel || relayIndex < 0 || relayIndex >= (int)rel.size()) return "NONE";
  JsonObject r = rel[relayIndex].as<JsonObject>();
  if(!r) return "NONE";
  JsonObject expr = r["expr"].as<JsonObject>();
  const char* op = (expr && !expr["op"].isNull()) ? (const char*)expr["op"] : "NONE";
  bool inv = r["invert"] | false;
  String out = "";
  if(inv) out += "INV ";

  if(strcmp(op,"NONE")==0){
    out += "NONE";
    return out;
  }
  if(strcmp(op,"FOLLOW")==0 || strcmp(op,"TOGGLE_RISE")==0 || strcmp(op,"PULSE_RISE")==0){
    int in = expr["in"] | 1;
    if(strcmp(op,"FOLLOW")==0) out += "FOLLOW E" + String(in);
    else if(strcmp(op,"TOGGLE_RISE")==0) out += "TOGGLE E" + String(in);
    else {
      uint32_t pulseMs = r["pulseMs"] | 200;
      out += "PULSE E" + String(in) + " " + String(pulseMs) + "ms";
    }
    return out;
  }
  if(strcmp(op,"AND")==0 || strcmp(op,"OR")==0 || strcmp(op,"XOR")==0){
    out += String(op) + " ";
    JsonArray ins = expr["ins"].as<JsonArray>();
    if(ins && ins.size()>0){
      for(size_t i=0;i<ins.size();i++){
        int in = (int)ins[i];
        if(i>0) out += ",";
        out += "E" + String(in);
      }
    } else {
      out += "E1";
    }
    return out;
  }
  out += String(op);
  return out;
}

static void mqttPublishDiscovery(const String& transport) {
  if (!mqttConnectedForTransport(transport)) return;
  if (mqttLowDataTransport(transport)) return; // data-saver on GSM
  String base = mqttBaseTopic();
  String node = mqttNodeId();
  String avail = base + "/status";
  String id = node;

  static JsonDocument doc;

  for (int i = 0; i < totalRelays; i++) {
    doc.clear();
    String uid = id + "_relay_" + String(i+1);
    doc["name"] = "Relay " + String(i+1);
    doc["uniq_id"] = uid;
    doc["stat_t"] = base + "/relay/" + String(i+1) + "/state";
    doc["cmd_t"] = base + "/relay/" + String(i+1) + "/set";
    doc["pl_on"] = "ON";
    doc["pl_off"] = "OFF";
    doc["avty_t"] = avail;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = id;
    dev["name"] = node;
    dev["mdl"] = "ESPRelay4";
    dev["mf"] = "ESPRelay4";
    String topic = mqttCfg.discoveryPrefix + "/switch/" + uid + "/config";
    String out; serializeJson(doc, out);
    mqttPublishToTransport(transport, topic, out, true);

    // Auto button to return relay to AUTO mode
    doc.clear();
    doc["name"] = "Relay " + String(i+1) + " AUTO";
    doc["uniq_id"] = uid + "_auto";
    doc["cmd_t"] = base + "/relay/" + String(i+1) + "/auto";
    doc["pl_press"] = "AUTO";
    doc["avty_t"] = avail;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    JsonObject dev2 = doc["dev"].to<JsonObject>();
    dev2["ids"] = id;
    dev2["name"] = node;
    dev2["mdl"] = "ESPRelay4";
    dev2["mf"] = "ESPRelay4";
    topic = mqttCfg.discoveryPrefix + "/button/" + uid + "_auto/config";
    serializeJson(doc, out);
    mqttPublishToTransport(transport, topic, out, true);

    // Rule summary sensor
    doc.clear();
    String rid = id + "_rule_" + String(i+1);
    doc["name"] = "Rule R" + String(i+1);
    doc["uniq_id"] = rid;
    doc["stat_t"] = base + "/rule/relay/" + String(i+1);
    doc["avty_t"] = avail;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    JsonObject devr = doc["dev"].to<JsonObject>();
    devr["ids"] = id;
    devr["name"] = node;
    devr["mdl"] = "ESPRelay4";
    devr["mf"] = "ESPRelay4";
    topic = mqttCfg.discoveryPrefix + "/sensor/" + rid + "/config";
    serializeJson(doc, out);
    mqttPublishToTransport(transport, topic, out, true);
  }

  // IP sensor
  doc.clear();
  String ipId = id + "_ip";
  doc["name"] = "IP";
  doc["uniq_id"] = ipId;
  doc["stat_t"] = base + "/net/ip";
  doc["avty_t"] = avail;
  doc["pl_avail"] = "online";
  doc["pl_not_avail"] = "offline";
  JsonObject devip = doc["dev"].to<JsonObject>();
  devip["ids"] = id;
  devip["name"] = node;
  devip["mdl"] = "ESPRelay4";
  devip["mf"] = "ESPRelay4";
  String ipTopic = mqttCfg.discoveryPrefix + "/sensor/" + ipId + "/config";
  String out; serializeJson(doc, out);
  mqttPublishToTransport(transport, ipTopic, out, true);

  // WiFi AP enable switch
  doc.clear();
  String wId = id + "_wifi_ap";
  doc["name"] = "WiFi AP";
  doc["uniq_id"] = wId;
  doc["stat_t"] = base + "/wifi/ap/state";
  doc["cmd_t"] = base + "/wifi/ap/set";
  doc["pl_on"] = "ON";
  doc["pl_off"] = "OFF";
  doc["avty_t"] = avail;
  doc["pl_avail"] = "online";
  doc["pl_not_avail"] = "offline";
  JsonObject devw = doc["dev"].to<JsonObject>();
  devw["ids"] = id;
  devw["name"] = node;
  devw["mdl"] = "ESPRelay4";
  devw["mf"] = "ESPRelay4";
  String wTopic = mqttCfg.discoveryPrefix + "/switch/" + wId + "/config";
  serializeJson(doc, out);
  mqttPublishToTransport(transport, wTopic, out, true);

  // BLE enable switch
  doc.clear();
  String bId = id + "_ble";
  doc["name"] = "BLE";
  doc["uniq_id"] = bId;
  doc["stat_t"] = base + "/ble/state";
  doc["cmd_t"] = base + "/ble/set";
  doc["pl_on"] = "ON";
  doc["pl_off"] = "OFF";
  doc["avty_t"] = avail;
  doc["pl_avail"] = "online";
  doc["pl_not_avail"] = "offline";
  JsonObject devb = doc["dev"].to<JsonObject>();
  devb["ids"] = id;
  devb["name"] = node;
  devb["mdl"] = "ESPRelay4";
  devb["mf"] = "ESPRelay4";
  String bTopic = mqttCfg.discoveryPrefix + "/switch/" + bId + "/config";
  serializeJson(doc, out);
  mqttPublishToTransport(transport, bTopic, out, true);

  for (int i = 0; i < totalInputs; i++) {
    doc.clear();
    String uid = id + "_input_" + String(i+1);
    doc["name"] = "Input " + String(i+1);
    doc["uniq_id"] = uid;
    doc["stat_t"] = base + "/input/" + String(i+1) + "/state";
    doc["pl_on"] = "ON";
    doc["pl_off"] = "OFF";
    doc["avty_t"] = avail;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = id;
    dev["name"] = node;
    dev["mdl"] = "ESPRelay4";
    dev["mf"] = "ESPRelay4";
    String topic = mqttCfg.discoveryPrefix + "/binary_sensor/" + uid + "/config";
    String out; serializeJson(doc, out);
    mqttPublishToTransport(transport, topic, out, true);
  }

  // Virtual inputs (MQTT-driven) as switches
  for (int i = 0; i < totalInputs; i++) {
    doc.clear();
    String uid = id + "_vin_" + String(i+1);
    doc["name"] = "VInput " + String(i+1);
    doc["uniq_id"] = uid;
    doc["stat_t"] = base + "/vin/" + String(i+1) + "/state";
    doc["cmd_t"] = base + "/vin/" + String(i+1) + "/set";
    doc["pl_on"] = "ON";
    doc["pl_off"] = "OFF";
    doc["avty_t"] = avail;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = id;
    dev["name"] = node;
    dev["mdl"] = "ESPRelay4";
    dev["mf"] = "ESPRelay4";
    String topic = mqttCfg.discoveryPrefix + "/switch/" + uid + "/config";
    String out; serializeJson(doc, out);
    mqttPublishToTransport(transport, topic, out, true);
  }

  for (int s = 0; s < shuttersLimit(); s++) {
    if (!shCfg[s].enabled) continue;
    doc.clear();
    String uid = id + "_shutter_" + String(s+1);
    doc["name"] = shCfg[s].name;
    doc["uniq_id"] = uid;
    doc["cmd_t"] = base + "/shutter/" + String(s+1) + "/set";
    doc["stat_t"] = base + "/shutter/" + String(s+1) + "/state";
    doc["pl_open"] = "OPEN";
    doc["pl_close"] = "CLOSE";
    doc["pl_stop"] = "STOP";
    doc["optimistic"] = true;
    doc["assumed_state"] = true;
    doc["avty_t"] = avail;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = id;
    dev["name"] = node;
    dev["mdl"] = "ESPRelay4";
    dev["mf"] = "ESPRelay4";
    String topic = mqttCfg.discoveryPrefix + "/cover/" + uid + "/config";
    String out; serializeJson(doc, out);
    mqttPublishToTransport(transport, topic, out, true);
  }

  // Temperature sensors
  for (int i = 0; i < tempCount; i++) {
    doc.clear();
    String uid = id + "_temp_" + String(i+1);
    doc["name"] = "Temp " + String(i+1);
    doc["uniq_id"] = uid;
    doc["stat_t"] = base + "/temp/" + String(i+1) + "/state";
    doc["unit_of_meas"] = "°C";
    doc["dev_cla"] = "temperature";
    doc["stat_cla"] = "measurement";
    doc["avty_t"] = avail;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = id;
    dev["name"] = node;
    dev["mdl"] = "ESPRelay4";
    dev["mf"] = "ESPRelay4";
    String topic = mqttCfg.discoveryPrefix + "/sensor/" + uid + "/config";
    String out; serializeJson(doc, out);
    mqttPublishToTransport(transport, topic, out, true);
  }

  if (dhtPresent) {
    doc.clear();
    String uid = id + "_temp_dht22";
    doc["name"] = "Temp DHT22";
    doc["uniq_id"] = uid;
    doc["stat_t"] = base + "/temp/dht/state";
    doc["unit_of_meas"] = "°C";
    doc["dev_cla"] = "temperature";
    doc["stat_cla"] = "measurement";
    doc["avty_t"] = avail;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = id;
    dev["name"] = node;
    dev["mdl"] = "ESPRelay4";
    dev["mf"] = "ESPRelay4";
    String topic = mqttCfg.discoveryPrefix + "/sensor/" + uid + "/config";
    String out; serializeJson(doc, out);
    mqttPublishToTransport(transport, topic, out, true);
  }
  if (dhtPresent) {
    doc.clear();
    String uid = id + "_hum_dht22";
    doc["name"] = "Humidité DHT22";
    doc["uniq_id"] = uid;
    doc["stat_t"] = base + "/hum/dht/state";
    doc["unit_of_meas"] = "%";
    doc["dev_cla"] = "humidity";
    doc["stat_cla"] = "measurement";
    doc["avty_t"] = avail;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = id;
    dev["name"] = node;
    dev["mdl"] = "ESPRelay4";
    dev["mf"] = "ESPRelay4";
    String topic = mqttCfg.discoveryPrefix + "/sensor/" + uid + "/config";
    String out; serializeJson(doc, out);
    mqttPublishToTransport(transport, topic, out, true);
  }

  if (transport == "gsm") mqttAnnouncedGsm = true;
  else mqttAnnouncedEth = true;
}

static void mqttPublishStateSnapshot(const String& transport, bool controlOnly) {
  if (!mqttConnectedForTransport(transport)) return;
  String base = mqttBaseTopic();
  mqttPublishToTransport(transport, base + "/status", "online", true);
  String ip = mqttCurrentIpForTransport(transport);
  mqttPublishToTransport(transport, base + "/net/ip", ip, mqttCfg.retain);
  if (transport == "gsm") lastIpPubGsm = ip;
  else lastIpPubEth = ip;
  mqttPublishToTransport(transport, base + "/gsm/iccid", gsmLastCcid.length() ? gsmLastCcid : "-", mqttCfg.retain);

  if (!controlOnly) {
    mqttPublishToTransport(transport, base + "/wifi/ap/state", wifiCfg.enabled ? "ON" : "OFF", mqttCfg.retain);
    lastWifiPub = wifiCfg.enabled;
    mqttPublishToTransport(transport, base + "/ble/state", bleEnabled ? "ON" : "OFF", mqttCfg.retain);
    lastBlePub = bleEnabled;
  }

  for (int i = 0; i < totalInputs; i++) {
    mqttPublishToTransport(transport, base + "/input/" + String(i+1) + "/state", inputs[i] ? "ON" : "OFF", mqttCfg.retain);
    lastInputsPub[i] = inputs[i];
  }
  for (int i = 0; i < totalInputs; i++) {
    mqttPublishToTransport(transport, base + "/vin/" + String(i+1) + "/state", virtualInputs[i] ? "ON" : "OFF", mqttCfg.retain);
    lastVirtualPub[i] = virtualInputs[i];
  }
  for (int i = 0; i < totalRelays; i++) {
    mqttPublishToTransport(transport, base + "/relay/" + String(i+1) + "/state", relays[i] ? "ON" : "OFF", mqttCfg.retain);
    lastRelaysPub[i] = relays[i];
    mqttPublishToTransport(transport, base + "/relay/" + String(i+1) + "/mode", relayModeText(overrideRelay[i]), mqttCfg.retain);
    lastRelayModePub[i] = overrideRelay[i];
  }
  if (!controlOnly) {
    for (int i = 0; i < totalRelays; i++) {
      String rs = ruleSummaryShort(i);
      mqttPublishToTransport(transport, base + "/rule/relay/" + String(i+1), rs, mqttCfg.retain);
      lastRulePub[i] = rs;
    }
  }
  for (int s = 0; s < shuttersLimit(); s++) {
    if (!shCfg[s].enabled) continue;
    const char* st = (shRt[s].move==SH_UP ? "opening" : (shRt[s].move==SH_DOWN ? "closing" : "stopped"));
    mqttPublishToTransport(transport, base + "/shutter/" + String(s+1) + "/state", String(st), mqttCfg.retain);
    lastShutterMove[s] = (int)shRt[s].move;
  }

  if (!controlOnly) {
    for (int i = 0; i < tempCount; i++) {
      if (tempC[i] > -100.0f) {
        mqttPublishToTransport(transport, base + "/temp/" + String(i+1) + "/state", String(tempC[i], 2), mqttCfg.retain);
        lastTempPub[i] = tempC[i];
      }
    }
    if (dhtPresent && !isnan(dhtTempC)) {
      mqttPublishToTransport(transport, base + "/temp/dht/state", String(dhtTempC, 2), mqttCfg.retain);
      lastDhtPub = dhtTempC;
    }
    if (dhtPresent && !isnan(dhtHum)) {
      mqttPublishToTransport(transport, base + "/hum/dht/state", String(dhtHum, 1), mqttCfg.retain);
      lastDhtHumPub = dhtHum;
    }
  }
}

static void mqttHandleMessage(const char* source, char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String p;
  bool fastCommand = false;
  for (unsigned int i = 0; i < length; i++) p += (char)payload[i];
  p.trim();
  p.toUpperCase();
  Serial.printf("[MQTT][%s] RX topic=%s payload=%s\n", source, t.c_str(), p.c_str());

  String base = mqttBaseTopic();
  if (t.startsWith(base + "/relay/") && t.endsWith("/auto")) {
    int idx = t.substring((base + "/relay/").length()).toInt();
    if (idx >= 1 && idx <= totalRelays) {
      int i = idx - 1;
      if (!reservedByShutter[i]) {
        overrideRelay[i] = -1;
        fastCommand = true;
      }
    }
  }
  else if (t == base + "/wifi/ap/set") {
    if (p == "ON") wifiCfg.enabled = true;
    else if (p == "OFF") wifiCfg.enabled = false;
    saveWifiCfg();
    applyWifiCfg();
  }
  else if (t == base + "/ble/set") {
    if (p == "ON") setBleEnabled(true);
    else if (p == "OFF") setBleEnabled(false);
    saveBleCfg();
  }
  else if (t.startsWith(base + "/vin/") && t.endsWith("/set")) {
    int idx = t.substring((base + "/vin/").length()).toInt();
    if (idx >= 1 && idx <= totalInputs) {
      int i = idx - 1;
      if (p == "ON") { virtualInputs[i] = true; fastCommand = true; }
      else if (p == "OFF") { virtualInputs[i] = false; fastCommand = true; }
      else if (p == "TOGGLE") { virtualInputs[i] = !virtualInputs[i]; fastCommand = true; }
    }
  }
  else if (t.startsWith(base + "/relay/") && t.endsWith("/set")) {
    int idx = t.substring((base + "/relay/").length()).toInt();
    if (idx >= 1 && idx <= totalRelays) {
      int i = idx - 1;
      if (!reservedByShutter[i]) {
        if (p == "ON") { overrideRelay[i] = 1; fastCommand = true; }
        else if (p == "OFF") { overrideRelay[i] = 0; fastCommand = true; }
        else if (p == "AUTO") { overrideRelay[i] = -1; fastCommand = true; }
        else if (p == "TOGGLE") { overrideRelay[i] = (overrideRelay[i] == 1 ? 0 : 1); fastCommand = true; }
      }
    }
  } else if (t.startsWith(base + "/shutter/") && t.endsWith("/set")) {
    int idx = t.substring((base + "/shutter/").length()).toInt();
    if (idx >= 1 && idx <= shuttersLimit()) {
      int s = idx - 1;
      if (p == "OPEN" || p == "UP") { shRt[s].manual = MC_UP; fastCommand = true; }
      else if (p == "CLOSE" || p == "DOWN") { shRt[s].manual = MC_DOWN; fastCommand = true; }
      else if (p == "STOP") { shRt[s].manual = MC_STOP; fastCommand = true; }
    }
  }
  if (fastCommand) {
    mqttFastCommandPending = true;
    mqttFastModeUntilMs = millis() + 700;
  }
}

static void mqttCallbackEth(char* topic, byte* payload, unsigned int length) {
  mqttHandleMessage("ETH", topic, payload, length);
}

static void mqttCallbackGsm(char* topic, byte* payload, unsigned int length) {
  mqttHandleMessage("GSM", topic, payload, length);
}

static void mqttSetup() {
  mqttCfg.transport = normalizeMqttTransport(mqttCfg.transport);
  mqttCfg.base = normalizeBaseTopic(mqttCfg.base);
  mqttApplyServerForTransport("ethernet");
  mqttApplyServerForTransport("gsm");
  mqttClientEth.setKeepAlive(MQTT_KEEPALIVE_SECONDS);
  mqttClientGsm.setKeepAlive(MQTT_KEEPALIVE_SECONDS);
  mqttClientEth.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SECONDS);
  mqttClientGsm.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SECONDS);
  mqttClientEth.setCallback(mqttCallbackEth);
  mqttClientGsm.setCallback(mqttCallbackGsm);
}

static void mqttSubscribeTopics(PubSubClient &client) {
  String base = mqttBaseTopic();
  for (int i = 1; i <= totalRelays; i++) {
    client.subscribe((base + "/relay/" + String(i) + "/set").c_str());
    client.subscribe((base + "/relay/" + String(i) + "/auto").c_str());
  }
  client.subscribe((base + "/wifi/ap/set").c_str());
  client.subscribe((base + "/ble/set").c_str());
  for (int i = 1; i <= totalInputs; i++) {
    client.subscribe((base + "/vin/" + String(i) + "/set").c_str());
  }
  for (int s = 1; s <= shuttersLimit(); s++) {
    client.subscribe((base + "/shutter/" + String(s) + "/set").c_str());
  }
}

static String mqttClientIdForTransport(const String& transport) {
  String id = mqttNodeId();
  if (transport == "gsm") {
    const String suffix = "-gsm";
    const size_t maxLen = 23;
    if (id.length() + suffix.length() > maxLen) {
      id.remove(maxLen - suffix.length());
    }
    id += suffix;
  }
  return id;
}

static bool mqttTryConnectTransport(const String& transport) {
  if (transport == "gsm") {
    if (!mqttTransportAllowsGsm()) return false;
    if (mqttGsmConnectedSafe()) return true;
  } else {
    if (!mqttTransportAllowsEthernet()) return false;
    if (mqttEthConnectedSafe()) return true;
  }

  PubSubClient *client = mqttClientForTransport(transport);

  uint32_t now = millis();
  if (transport == "gsm") {
    if (now - mqttLastConnectGsmMs < 3000) return false;
    mqttLastConnectGsmMs = now;
  } else {
    if (now - mqttLastConnectEthMs < 3000) return false;
    mqttLastConnectEthMs = now;
  }

  if (transport == "ethernet") {
    if (!mqttTransportAllowsEthernet()) return false;
    if (Ethernet.linkStatus() != LinkON) return false;
    if (!mqttShouldTryEthernetNow()) return false;
  } else {
    if (!mqttTransportAllowsGsm()) return false;
    if (!gsmEnsureData()) {
      Serial.println("[MQTT][GSM] skip connect: 1NCE not ready");
      return false;
    }
  }

  mqttApplyServerForTransport(transport);
  String willTopic = mqttBaseTopic() + "/status";

  const char* mqttHost = mqttHostForTransport(transport);
  uint16_t mqttPort = mqttPortForTransport(transport);
  String mqttHostStr = String(mqttHost);
  String mqttUser = mqttUserForTransport(transport);
  String mqttPass = mqttPassForTransport(transport);
  if (transport == "gsm" && mqttHostLooksPrivateForGsm(mqttHostStr)) {
    Serial.printf("[MQTT][GSM] WARN host '%s' semble prive, utiliser gsm_mqtt_host public\n", mqttHost);
  }

  String clientId = mqttClientIdForTransport(transport);
  Serial.printf("[MQTT][%s] connect %s:%u client=%s\n",
                (transport == "gsm") ? "GSM" : "ETH",
                mqttHost, mqttPort, clientId.c_str());

  bool ok = false;
  if (mqttUser.length() > 0) {
    ok = client->connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str(),
                         willTopic.c_str(), 0, true, "offline");
  } else {
    ok = client->connect(clientId.c_str(), willTopic.c_str(), 0, true, "offline");
  }

  if (ok) {
    if (transport == "gsm") {
      Serial.println("[MQTT][GSM] connected via modem");
      gsmDebug1nce("mqtt_connected", true);
    } else {
      Serial.println("[MQTT][ETH] connected");
      mqttLastEthFailMs = 0;
      mqttLastEthFailRc = 0;
      mqttEthAuthBlocked = false;
    }
    mqttSubscribeTopics(*client);
    if (mqttLowDataTransport(transport)) {
      Serial.println("[MQTT][GSM] low-data mode: publish control state only");
      mqttPublishStateSnapshot(transport, true);
      mqttAnnouncedGsm = true;
      if (!mqttAnnouncedEth) {
        Serial.println("[MQTT][GSM] discovery skipped (data saver)");
      }
    } else {
      mqttPublishStateSnapshot(transport, false);
      mqttPublishDiscovery(transport);
    }
    return true;
  }

  if (transport == "gsm" && !modem.isGprsConnected()) {
    gsmDataReady = false;
  }
  if (transport == "gsm") gsmDebug1nce("mqtt_connect_fail", true);
  int rc = client->state();
  if (transport == "ethernet") {
    mqttEth.stop(); // release W5500 socket immediately on failed MQTT connect
    mqttLastEthFailMs = millis();
    mqttLastEthFailRc = rc;
    if (rc == 4 || rc == 5) {
      if (!mqttEthAuthBlocked) {
        Serial.println("[MQTT][ETH] auth rejected -> cooldown enabled");
      }
      mqttEthAuthBlocked = true;
    }
  }
  Serial.printf("[MQTT][%s] connect failed rc=%d (%s)\n",
                (transport == "gsm") ? "GSM" : "ETH",
                rc, mqttStateText(rc));
  return false;
}

static void mqttEnsureConnected() {
  if (!mqttCfg.enabled) return;
  if (mqttTransportAllowsEthernet()) {
    mqttTryConnectTransport("ethernet");
  }
  if (mqttTransportAllowsGsm()) {
    mqttTryConnectTransport("gsm");
  }
}

static void mqttLoop() {
  if (millis() < MQTT_STARTUP_GRACE_MS) {
    if (!mqttStartupGraceLogged) {
      Serial.printf("[MQTT] startup grace %lus: HTTP priority\n", (unsigned long)(MQTT_STARTUP_GRACE_MS / 1000));
      mqttStartupGraceLogged = true;
    }
    return;
  }

  if (!mqttCfg.enabled) {
    if (!mqttDisabledWarned) {
      Serial.println("[MQTT] disabled in config -> no connection attempt");
      mqttDisabledWarned = true;
    }
    if (mqttEthConnectedSafe()) mqttClientEth.disconnect();
    if (mqttGsmConnectedSafe()) mqttClientGsm.disconnect();
    return;
  }
  mqttDisabledWarned = false;

  bool ethConn = mqttEthConnectedSafe();
  bool gsmConn = mqttGsmConnectedSafe();

  if (!mqttTransportAllowsEthernet() && ethConn) {
    mqttClientEth.disconnect();
    ethConn = false;
  }
  if (!mqttTransportAllowsGsm() && gsmConn) {
    mqttClientGsm.disconnect();
    gsmConn = false;
  }

  mqttEnsureConnected();
  ethConn = mqttEthConnectedSafe();
  gsmConn = mqttGsmConnectedSafe();
  if (ethConn) mqttClientEth.loop();
  if (gsmConn) mqttClientGsm.loop();

  if (mqttTransportAllowsGsm()) {
    gsmDebug1nce(gsmConn ? "loop" : "mqtt_disconnected", false);
  }

  // Prioritize command actuation over telemetry flood.
  if (mqttFastCommandPending || millis() < mqttFastModeUntilMs) return;

  if (!ethConn && !gsmConn) return;

  if (ethConn && !mqttAnnouncedEth) {
    mqttPublishDiscovery("ethernet");
  }

  String base = mqttBaseTopic();
  if (ethConn) {
    String ipEth = mqttCurrentIpForTransport("ethernet");
    if(ipEth != lastIpPubEth){
      mqttPublishToTransport("ethernet", base + "/net/ip", ipEth, mqttCfg.retain);
      lastIpPubEth = ipEth;
    }
  }
  if (gsmConn) {
    String ipGsm = mqttCurrentIpForTransport("gsm");
    if(ipGsm != lastIpPubGsm){
      mqttPublishToTransport("gsm", base + "/net/ip", ipGsm, mqttCfg.retain);
      lastIpPubGsm = ipGsm;
    }
  }

  if (ethConn) {
    if(wifiCfg.enabled != lastWifiPub){
      mqttPublishEthernetOnly(base + "/wifi/ap/state", wifiCfg.enabled ? "ON" : "OFF", mqttCfg.retain);
      lastWifiPub = wifiCfg.enabled;
    }
    if(bleEnabled != lastBlePub){
      mqttPublishEthernetOnly(base + "/ble/state", bleEnabled ? "ON" : "OFF", mqttCfg.retain);
      lastBlePub = bleEnabled;
    }
  }

  static uint32_t lastRuleCheckMs = 0;
  const uint32_t now = millis();
  if (ethConn && (now - lastRuleCheckMs > 2000)) {
    lastRuleCheckMs = now;
    for (int i = 0; i < totalRelays; i++) {
      String rs = ruleSummaryShort(i);
      if(rs != lastRulePub[i]){
        mqttPublishEthernetOnly(base + "/rule/relay/" + String(i+1), rs, mqttCfg.retain);
        lastRulePub[i] = rs;
      }
    }
  }
  for (int i = 0; i < totalInputs; i++) {
    if (inputs[i] != lastInputsPub[i]) {
      mqttPublish(base + "/input/" + String(i+1) + "/state", inputs[i] ? "ON" : "OFF", mqttCfg.retain);
      lastInputsPub[i] = inputs[i];
    }
  }
  for (int i = 0; i < totalInputs; i++) {
    if (virtualInputs[i] != lastVirtualPub[i]) {
      mqttPublish(base + "/vin/" + String(i+1) + "/state", virtualInputs[i] ? "ON" : "OFF", mqttCfg.retain);
      lastVirtualPub[i] = virtualInputs[i];
    }
  }
  for (int i = 0; i < totalRelays; i++) {
    if (relays[i] != lastRelaysPub[i]) {
      mqttPublish(base + "/relay/" + String(i+1) + "/state", relays[i] ? "ON" : "OFF", mqttCfg.retain);
      lastRelaysPub[i] = relays[i];
    }
    if (overrideRelay[i] != lastRelayModePub[i]) {
      mqttPublish(base + "/relay/" + String(i+1) + "/mode", relayModeText(overrideRelay[i]), mqttCfg.retain);
      lastRelayModePub[i] = overrideRelay[i];
    }
  }

  for (int s = 0; s < shuttersLimit(); s++) {
    if (!shCfg[s].enabled) continue;
    if ((int)shRt[s].move != lastShutterMove[s]) {
      const char* st = (shRt[s].move==SH_UP ? "opening" : (shRt[s].move==SH_DOWN ? "closing" : "stopped"));
      mqttPublish(base + "/shutter/" + String(s+1) + "/state", String(st), mqttCfg.retain);
      lastShutterMove[s] = (int)shRt[s].move;
    }
  }

  if (ethConn) {
    for (int i = 0; i < tempCount; i++) {
      if (fabs(tempC[i] - lastTempPub[i]) >= 0.1f) {
        mqttPublishEthernetOnly(base + "/temp/" + String(i+1) + "/state", String(tempC[i], 2), mqttCfg.retain);
        lastTempPub[i] = tempC[i];
      }
    }

    if (dhtPresent && !isnan(dhtTempC)) {
      if (isnan(lastDhtPub) || fabs(dhtTempC - lastDhtPub) >= 0.1f) {
        mqttPublishEthernetOnly(base + "/temp/dht/state", String(dhtTempC, 2), mqttCfg.retain);
        lastDhtPub = dhtTempC;
      }
    }
    if (dhtPresent && !isnan(dhtHum)) {
      if (isnan(lastDhtHumPub) || fabs(dhtHum - lastDhtHumPub) >= 0.5f) {
        mqttPublishEthernetOnly(base + "/hum/dht/state", String(dhtHum, 1), mqttCfg.retain);
        lastDhtHumPub = dhtHum;
      }
    }
  }
}

static bool inRangeInput(int v){ return v>=1 && v<=totalInputs; }
static bool inRangeRelay(int v){ return v>=1 && v<=totalRelays; }
static uint8_t shuttersLimit(){ return (uint8_t)min((int)SHUTTER_MAX, (int)(totalRelays/2)); }
static uint8_t shuttersLimit();

static void clearReservations() {
  for(int i=0;i<MAX_RELAYS;i++) reservedByShutter[i] = false;
}

static void applyReservationsFromConfig() {
  clearReservations();
  for (int s = 0; s < shuttersLimit(); s++) {
    if(!shCfg[s].enabled) continue;
    if(inRangeRelay(shCfg[s].up_relay)) reservedByShutter[shCfg[s].up_relay-1] = true;
    if(inRangeRelay(shCfg[s].down_relay)) reservedByShutter[shCfg[s].down_relay-1] = true;
  }
}

static bool parseShutterFromRules(JsonArray shutters, String &errMsg) {
  for(int i=0;i<SHUTTER_MAX;i++){
    shCfg[i] = ShutterCfg();
    shRt[i] = ShutterRuntime();
    shCfg[i].enabled = false;
  }

  if(!shutters || shutters.size()==0){
    applyReservationsFromConfig();
    return true;
  }

  int count = (int)shutters.size();
  int limit = shuttersLimit();
  if(count > limit) count = limit;

  for(int s=0; s<count; s++){
    JsonObject so = shutters[s].as<JsonObject>();
    if(!so) continue;

    shCfg[s].enabled = true;
    shCfg[s].name = (const char*)(so["name"] | (s==0 ? "Volet 1" : "Volet 2"));

    shCfg[s].up_in = (uint8_t)(int)(so["up_in"] | 1);
    shCfg[s].down_in = (uint8_t)(int)(so["down_in"] | 2);
    shCfg[s].up_relay = (uint8_t)(int)(so["up_relay"] | (s==0 ? 1 : 3));
    shCfg[s].down_relay = (uint8_t)(int)(so["down_relay"] | (s==0 ? 2 : 4));

    shCfg[s].mode = (const char*)(so["mode"] | "hold");
    shCfg[s].priority = (const char*)(so["priority"] | "stop");

    shCfg[s].deadtime_ms = (uint32_t)(uint64_t)(so["deadtime_ms"] | 400);
    shCfg[s].max_run_ms  = (uint32_t)(uint64_t)(so["max_run_ms"] | 25000);

    if(!inRangeInput(shCfg[s].up_in) || !inRangeInput(shCfg[s].down_in)){
      errMsg = String("shutter ") + String(s+1) + ": up_in/down_in out of range";
      return false;
    }
    if(!inRangeRelay(shCfg[s].up_relay) || !inRangeRelay(shCfg[s].down_relay)){
      errMsg = String("shutter ") + String(s+1) + ": up_relay/down_relay out of range";
      return false;
    }
    if(shCfg[s].up_relay == shCfg[s].down_relay){
      errMsg = String("shutter ") + String(s+1) + ": up_relay and down_relay must be different";
      return false;
    }
    if(!(shCfg[s].mode=="hold" || shCfg[s].mode=="toggle")){
      errMsg = String("shutter ") + String(s+1) + ": mode must be hold|toggle";
      return false;
    }
    if(!(shCfg[s].priority=="stop" || shCfg[s].priority=="up" || shCfg[s].priority=="down")){
      errMsg = String("shutter ") + String(s+1) + ": priority must be stop|up|down";
      return false;
    }
    if(shCfg[s].deadtime_ms > 60000) shCfg[s].deadtime_ms = 60000;
    if(shCfg[s].max_run_ms > 600000) shCfg[s].max_run_ms = 600000;
  }

  // check relay conflicts between shutters
  for(int a=0;a<limit;a++){
    if(!shCfg[a].enabled) continue;
    for(int b=a+1;b<limit;b++){
      if(!shCfg[b].enabled) continue;
      if(shCfg[a].up_relay==shCfg[b].up_relay || shCfg[a].up_relay==shCfg[b].down_relay ||
         shCfg[a].down_relay==shCfg[b].up_relay || shCfg[a].down_relay==shCfg[b].down_relay){
        errMsg = "shutters conflict: relays overlap";
        return false;
      }
    }
  }

  applyReservationsFromConfig();
  return true;
}

static bool getInputN(int n){ // n = 1..totalInputs
  if(n < 1 || n > totalInputs) return false;
  return combinedInputs[n-1];
}

static void shutterSetOutputs(int s, ShutterMove m) {
  // sécurité absolue: jamais les deux
  bool up = (m == SH_UP);
  bool dn = (m == SH_DOWN);

  // si interlock violé (ne devrait jamais) => STOP
  if(up && dn){
    up = false; dn = false; m = SH_STOP;
  }

  if(!shCfg[s].enabled) return;
  relayFromShutter[shCfg[s].up_relay-1] = up;
  relayFromShutter[shCfg[s].down_relay-1] = dn;
}

static void shutterForceStop(int s) {
  shRt[s].move = SH_STOP;
  shRt[s].manual = MC_NONE;
  shutterSetOutputs(s, SH_STOP);
}

static void shutterCommand(int s, ShutterMove req) {
  // gestion dead-time entre inversions
  uint32_t now = millis();

  if(req == SH_STOP){
    shRt[s].move = SH_STOP;
    shutterSetOutputs(s, SH_STOP);
    return;
  }

  // si cooldown actif, on reste STOP jusqu’à expiration
  if(now < shRt[s].cooldownUntilMs){
    shRt[s].move = SH_STOP;
    shutterSetOutputs(s, SH_STOP);
    return;
  }

  // si changement de sens alors qu’on bouge -> passer STOP + cooldown
  if(shRt[s].move != SH_STOP && shRt[s].move != req){
    shRt[s].move = SH_STOP;
    shutterSetOutputs(s, SH_STOP);
    shRt[s].cooldownUntilMs = now + shCfg[s].deadtime_ms;
    return; // la prochaine itération autorisera req après cooldown
  }

  // sinon, démarrer/continuer
  if(shRt[s].move != req){
    shRt[s].move = req;
    shRt[s].moveStartMs = now;
  }

  shutterSetOutputs(s, req);
}

static ShutterMove shutterComputeDemandFromButtons(int s) {
  bool upBtn = getInputN(shCfg[s].up_in);
  bool dnBtn = getInputN(shCfg[s].down_in);

  // priorité si les deux
  if(upBtn && dnBtn){
    if(shCfg[s].priority=="up") return SH_UP;
    if(shCfg[s].priority=="down") return SH_DOWN;
    return SH_STOP; // stop
  }
  if(upBtn) return SH_UP;
  if(dnBtn) return SH_DOWN;
  return SH_STOP;
}

static void shutterTickOne(int s) {
  if(!shCfg[s].enabled) return;

  uint32_t now = millis();

  if(shCfg[s].max_run_ms > 0 && shRt[s].move != SH_STOP){
    if(now - shRt[s].moveStartMs >= shCfg[s].max_run_ms){
      shutterForceStop(s);
      return;
    }
  }

  ShutterMove demand = SH_STOP;

  if(shRt[s].manual == MC_STOP){
    shutterForceStop(s);
    return;
  }
  if(shRt[s].manual == MC_UP) demand = SH_UP;
  else if(shRt[s].manual == MC_DOWN) demand = SH_DOWN;
  else {
    if(shCfg[s].mode == "hold"){
      demand = shutterComputeDemandFromButtons(s);
    } else {
      bool upBtn = getInputN(shCfg[s].up_in);
      bool dnBtn = getInputN(shCfg[s].down_in);

      bool upRise = upBtn && !shRt[s].lastUpBtn;
      bool dnRise = dnBtn && !shRt[s].lastDownBtn;

      shRt[s].lastUpBtn = upBtn;
      shRt[s].lastDownBtn = dnBtn;

      if(upRise && dnRise){
        demand = SH_STOP;
        shutterCommand(s, SH_STOP);
        return;
      }

      if(upRise){
        if(shRt[s].move == SH_UP) demand = SH_STOP;
        else demand = SH_UP;
      } else if(dnRise){
        if(shRt[s].move == SH_DOWN) demand = SH_STOP;
        else demand = SH_DOWN;
      } else {
        demand = shRt[s].move;
      }
    }
  }

  if(shCfg[s].mode=="hold" && shRt[s].manual==MC_NONE){
    shutterCommand(s, demand);
    return;
  }

  shutterCommand(s, demand);
}

static void shutterTick() {
  for(int i=0;i<MAX_RELAYS;i++) relayFromShutter[i] = false;
  for(int s=0; s<shuttersLimit(); s++){
    shutterTickOne(s);
  }
}

// ===============================================================
// Simple rules engine
// ===============================================================
static bool applyDelays(int i, bool desired, uint32_t onDelay, uint32_t offDelay) {
  if(onDelay==0 && offDelay==0){
    hasPending[i] = false;
    return desired;
  }

  if(!hasPending[i] || pendingTarget[i] != desired){
    pendingTarget[i] = desired;
    hasPending[i] = true;
    uint32_t d = desired ? onDelay : offDelay;
    pendingDeadlineMs[i] = millis() + d;
  }

  uint32_t d = desired ? onDelay : offDelay;
  if(d == 0){
    hasPending[i] = false;
    return desired;
  }

  if(millis() >= pendingDeadlineMs[i]){
    hasPending[i] = false;
    return desired;
  }
  return relayFromSimple[i];
}

static bool evalExprSimple(int relayIndex, JsonObject expr, uint32_t rulePulseMs) {
  const char* op = expr["op"] | "FOLLOW";

  if(strcmp(op, "NONE")==0){
    return false;
  }
  if(strcmp(op, "FOLLOW")==0){
    int in = expr["in"] | 1;
    return getInputN(in);
  }
  if(strcmp(op, "AND")==0 || strcmp(op,"OR")==0 || strcmp(op,"XOR")==0){
    JsonArray ins = expr["ins"].as<JsonArray>();
    if(!ins || ins.size()==0) return false;
    bool acc = (strcmp(op,"AND")==0) ? true : false;
    bool x = false;
    for(JsonVariant v : ins){
      int in = (int)v;
      bool b = getInputN(in);
      if(strcmp(op,"AND")==0) acc &= b;
      else if(strcmp(op,"OR")==0) acc |= b;
      else x ^= b;
    }
    return (strcmp(op,"XOR")==0) ? x : acc;
  }
  if(strcmp(op,"TOGGLE_RISE")==0){
    int in = expr["in"] | 1;
    bool thisRise = false;
    if(in>=1 && in<=totalInputs){
      thisRise = (combinedInputs[in-1] && !prevCombinedInputs[in-1]);
    }
    if(thisRise) toggleState[relayIndex] = !toggleState[relayIndex];
    return toggleState[relayIndex];
  }
  if(strcmp(op,"PULSE_RISE")==0){
    int in = expr["in"] | 1;
    uint32_t pulseMs = expr["pulseMs"] | rulePulseMs;
    if(pulseMs == 0) pulseMs = 1;
    bool thisRise = false;
    if(in>=1 && in<=totalInputs){
      thisRise = (combinedInputs[in-1] && !prevCombinedInputs[in-1]);
    }
    if(thisRise){
      pulseUntilMs[relayIndex] = millis() + pulseMs;
    }
    return (millis() < pulseUntilMs[relayIndex]);
  }

  return false;
}

static void evalSimpleRules() {
  JsonArray rel = rulesDoc["relays"].as<JsonArray>();
  for(int i=0;i<totalRelays;i++){
    bool desired = false;

    if(rel && i < (int)rel.size()){
      JsonObject r = rel[i].as<JsonObject>();
      JsonObject expr = r["expr"].as<JsonObject>();
      uint32_t pulseMs = r["pulseMs"] | 200;
      desired = evalExprSimple(i, expr, pulseMs);

      bool inv = r["invert"] | false;
      if(inv) desired = !desired;

      uint32_t onD  = r["onDelay"] | 0;
      uint32_t offD = r["offDelay"] | 0;

      desired = applyDelays(i, desired, onD, offD);
    }

    relayFromSimple[i] = desired;
  }
}

static void buildFinalRelays() {
  // 1) base = simple rules
  for(int i=0;i<totalRelays;i++){
    relays[i] = relayFromSimple[i];
  }

  // 2) apply shutter ownership: for each reserved relay, shutter output wins
  for(int s=0; s<shuttersLimit(); s++){
    if(!shCfg[s].enabled) continue;
    relays[shCfg[s].up_relay-1]   = relayFromShutter[shCfg[s].up_relay-1];
    relays[shCfg[s].down_relay-1] = relayFromShutter[shCfg[s].down_relay-1];
  }

  // 3) apply override ONLY for non-reserved relays
  for(int i=0;i<totalRelays;i++){
    if(reservedByShutter[i]) continue; // PROTECTION: cannot override shutter relays
    if(overrideRelay[i] == -1) continue;
    relays[i] = (overrideRelay[i] == 1);
  }

  // 4) final safety (absolute): if shutter relays both ON => STOP both
  for(int s=0; s<shuttersLimit(); s++){
    if(!shCfg[s].enabled) continue;
    bool up = relays[shCfg[s].up_relay-1];
    bool dn = relays[shCfg[s].down_relay-1];
    if(up && dn){
      relays[shCfg[s].up_relay-1] = false;
      relays[shCfg[s].down_relay-1] = false;
    }
  }

}

// ===============================================================
// HTTP helpers
// ===============================================================
static String readLine(Client& c){
  static const uint32_t HTTP_IO_TIMEOUT_MS = 800;
  String s;
  uint32_t lastRxMs = millis();
  while(c.connected() || c.available()){
    if(c.available()){
      char ch = c.read();
      if(ch=='\n') break;
      if(ch!='\r') s += ch;
      lastRxMs = millis();
    } else {
      if ((millis() - lastRxMs) > HTTP_IO_TIMEOUT_MS) break;
      delay(1);
    }
  }
  return s;
}

static void sendText(Client& c, const String& body, const char* ctype, int code);

static bool clientWriteAll(Client& c, const uint8_t* data, size_t len, uint32_t timeoutMs){
  size_t off = 0;
  uint32_t lastProgress = millis();
  while (off < len) {
    int w = c.write(data + off, len - off);
    if (w > 0) {
      off += (size_t)w;
      lastProgress = millis();
      continue;
    }
    if ((millis() - lastProgress) > timeoutMs) return false;
    delay(1);
  }
  return true;
}

static bool clientWriteString(Client& c, const String& s, uint32_t timeoutMs){
  if (s.length() == 0) return true;
  return clientWriteAll(c, (const uint8_t*)s.c_str(), s.length(), timeoutMs);
}

static String base64Decode(const String& in){
  static int8_t table[256];
  static bool inited = false;
  if(!inited){
    for(int i=0;i<256;i++) table[i] = -1;
    const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(int i=0;i<64;i++) table[(uint8_t)alpha[i]] = i;
    inited = true;
  }
  String out;
  out.reserve((in.length()*3)/4);
  int val = 0;
  int valb = -8;
  for(size_t i=0;i<in.length();i++){
    int8_t c = table[(uint8_t)in[i]];
    if(c < 0) continue;
    val = (val<<6) + c;
    valb += 6;
    if(valb >= 0){
      out += char((val>>valb) & 0xFF);
      valb -= 8;
    }
  }
  return out;
}

static bool checkAuthHeader(const String& authHeader){
  if(authHeader.length() == 0) return false;
  String h = authHeader;
  h.trim();
  if(!h.startsWith("Basic ")) return false;
  String b64 = h.substring(6);
  b64.trim();
  String decoded = base64Decode(b64);
  int sep = decoded.indexOf(':');
  if(sep < 0) return false;
  String user = decoded.substring(0, sep);
  String pass = decoded.substring(sep+1);
  return (user == authCfg.user && pass == authCfg.pass);
}

static void sendAuthRequired(Client& c){
  sendText(c, String("{\"ok\":false,\"error\":\"auth required\"}"), "application/json", 401);
}

static String readBody(Client& c, int len){
  static const uint32_t HTTP_IO_TIMEOUT_MS = 2000;
  String b; b.reserve(len);
  uint32_t lastRxMs = millis();
  while(len-- > 0){
    while(!c.available()){
      if (!c.connected() || (millis() - lastRxMs) > HTTP_IO_TIMEOUT_MS) {
        return b;
      }
      delay(1);
    }
    b += (char)c.read();
    lastRxMs = millis();
  }
  return b;
}

static void sendText(Client& c, const String& body, const char* ctype, int code=200){
  String status = "HTTP/1.1 500 Internal Server Error";
  if(code==200) status = "HTTP/1.1 200 OK";
  else if(code==401) status = "HTTP/1.1 401 Unauthorized";
  else if(code==204) status = "HTTP/1.1 204 No Content";
  else if(code==400) status = "HTTP/1.1 400 Bad Request";
  else if(code==404) status = "HTTP/1.1 404 Not Found";

  String hdr = status + "\r\n";
  hdr += "Content-Type: ";
  hdr += ctype;
  hdr += "\r\n";
  hdr += "Connection: close\r\n";
  hdr += "Content-Length: " + String(body.length()) + "\r\n\r\n";
  if (!clientWriteString(c, hdr, 4000)) {
    return;
  }
  if (body.length() > 0) {
    clientWriteAll(c, (const uint8_t*)body.c_str(), body.length(), 4000);
  }
  c.flush();
  delay(2);
}

static void buildStateJson(String &out){
  static StaticJsonDocument<4096> doc;
  doc.clear();
  JsonArray inA = doc["inputs"].to<JsonArray>();
  JsonArray reA = doc["relays"].to<JsonArray>();
  JsonArray ovA = doc["override"].to<JsonArray>();
  JsonArray rsA = doc["reserved"].to<JsonArray>();
  JsonArray modA = doc["modules_status"].to<JsonArray>();
  JsonArray modF = doc["modules_fail"].to<JsonArray>();

  for(int i=0;i<totalInputs;i++){
    inA.add(inputs[i] ? 1 : 0);
  }
  for(int i=0;i<totalRelays;i++){
    reA.add(relays[i] ? 1 : 0);
    ovA.add(overrideRelay[i]);
    rsA.add(reservedByShutter[i] ? 1 : 0);
  }
  for(int m=0; m<pcaCount; m++){
    const bool ok = (pcaLastOkMs[m] != 0) && (millis() - pcaLastOkMs[m] < 5000);
    modA.add(ok ? 1 : 0);
    modF.add(pcaFailCount[m]);
  }

  JsonObject eth = doc["eth"].to<JsonObject>();
  eth["link"] = (Ethernet.linkStatus()==LinkON) ? 1 : 0;
  eth["ip"] = Ethernet.localIP().toString();

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["enabled"] = wifiCfg.enabled ? 1 : 0;
  wifi["ap"] = wifiApOn ? 1 : 0;
  wifi["ssid"] = wifiCfg.ssid;
  wifi["ip"] = wifiApOn ? WiFi.softAPIP().toString() : "";

  JsonObject mq = doc["mqtt"].to<JsonObject>();
  mq["enabled"] = mqttCfg.enabled ? 1 : 0;
  const bool ethConn = mqttEthConnectedSafe();
  const bool gsmConn = mqttGsmConnectedSafe();
  mq["connected"] = (ethConn || gsmConn) ? 1 : 0;
  mq["eth_connected"] = ethConn ? 1 : 0;
  mq["gsm_connected"] = gsmConn ? 1 : 0;
  mq["transport"] = normalizeMqttTransport(mqttCfg.transport);
  mq["active_transport"] = mqttActiveTransportText();
  mq["gsm_network"] = gsmNetworkReady ? 1 : 0;
  mq["gsm_data"] = gsmDataReady ? 1 : 0;
  mq["ip"] = mqttCurrentIp();

  JsonObject sh = doc["shutter"].to<JsonObject>();
  sh["enabled"] = shCfg[0].enabled ? 1 : 0;
  if(shCfg[0].enabled){
    sh["name"] = shCfg[0].name;
    sh["up_relay"] = shCfg[0].up_relay;
    sh["down_relay"] = shCfg[0].down_relay;
    sh["move"] = (shRt[0].move==SH_UP ? "up" : (shRt[0].move==SH_DOWN ? "down" : "stop"));
    sh["cooldown_ms"] = (millis() < shRt[0].cooldownUntilMs) ? (uint32_t)(shRt[0].cooldownUntilMs - millis()) : 0;
  }

  JsonArray shA = doc["shutters"].to<JsonArray>();
  for(int s=0; s<shuttersLimit(); s++){
    JsonObject o = shA.add<JsonObject>();
    o["enabled"] = shCfg[s].enabled ? 1 : 0;
    if(shCfg[s].enabled){
      o["name"] = shCfg[s].name;
      o["up_relay"] = shCfg[s].up_relay;
      o["down_relay"] = shCfg[s].down_relay;
      o["move"] = (shRt[s].move==SH_UP ? "up" : (shRt[s].move==SH_DOWN ? "down" : "stop"));
      o["cooldown_ms"] = (millis() < shRt[s].cooldownUntilMs) ? (uint32_t)(shRt[s].cooldownUntilMs - millis()) : 0;
    }
  }

  JsonArray tA = doc["temps"].to<JsonArray>();
  for(int i=0;i<tempCount;i++){
    JsonObject t = tA.add<JsonObject>();
    t["addr"] = tempAddrToString(tempAddr[i]);
    t["c"] = tempC[i];
  }
  if(dhtPresent && (!isnan(dhtTempC) || !isnan(dhtHum))){
    JsonObject t = tA.add<JsonObject>();
    t["addr"] = "DHT22";
    if(!isnan(dhtTempC)) t["c"] = dhtTempC;
    if(!isnan(dhtHum)) t["h"] = dhtHum;
  }

  doc["modules"] = pcaCount;
  doc["relays_per"] = RELAYS_PER_MODULE;
  doc["inputs_per"] = INPUTS_PER_MODULE;
  doc["total_relays"] = totalRelays;
  doc["total_inputs"] = totalInputs;
  doc["fw"] = FW_VERSION;
  if(strlen(FW_TAG) > 0) doc["fw_tag"] = FW_TAG;
  doc["uptime_ms"] = (uint32_t)millis();

  if (serializeJson(doc, out) == 0) {
    out = "{}";
  }
}

static void sendJsonState(Client& c){
  String out;
  buildStateJson(out);
  sendText(c, out, "application/json");
}

static void buildStateJsonBle(String &out){
  static StaticJsonDocument<1536> doc;
  doc.clear();
  JsonArray inA = doc["inputs"].to<JsonArray>();
  JsonArray reA = doc["relays"].to<JsonArray>();
  JsonArray ovA = doc["override"].to<JsonArray>();
  JsonArray modA = doc["modules_status"].to<JsonArray>();
  JsonArray modF = doc["modules_fail"].to<JsonArray>();

  for(int i=0;i<totalInputs;i++){
    inA.add(inputs[i] ? 1 : 0);
  }
  for(int i=0;i<totalRelays;i++){
    reA.add(relays[i] ? 1 : 0);
    ovA.add(overrideRelay[i]);
  }
  for(int m=0; m<pcaCount; m++){
    const bool ok = (pcaLastOkMs[m] != 0) && (millis() - pcaLastOkMs[m] < 5000);
    modA.add(ok ? 1 : 0);
    modF.add(pcaFailCount[m]);
  }

  JsonObject eth = doc["eth"].to<JsonObject>();
  eth["link"] = (Ethernet.linkStatus()==LinkON) ? 1 : 0;
  eth["ip"] = Ethernet.localIP().toString();

  JsonObject mq = doc["mqtt"].to<JsonObject>();
  mq["enabled"] = mqttCfg.enabled ? 1 : 0;
  const bool ethConn = mqttEthConnectedSafe();
  const bool gsmConn = mqttGsmConnectedSafe();
  mq["connected"] = (ethConn || gsmConn) ? 1 : 0;
  mq["eth_connected"] = ethConn ? 1 : 0;
  mq["gsm_connected"] = gsmConn ? 1 : 0;
  mq["active_transport"] = mqttActiveTransportText();
  mq["on_gsm"] = gsmConn ? 1 : 0;

  JsonObject gsm = doc["gsm"].to<JsonObject>();
  gsm["modem_ready"] = modemReady ? 1 : 0;
  gsm["ready"] = (modemReady && gsmNetworkReady && gsmDataReady) ? 1 : 0;
  gsm["network"] = gsmNetworkReady ? 1 : 0;
  gsm["data"] = gsmDataReady ? 1 : 0;
  gsm["csq"] = gsmLastCsq;
  gsm["dbm"] = gsmLastDbm;
  gsm["op"] = gsmLastOperator;
  gsm["apn"] = gsmLastApn;
  gsm["ip"] = gsmLastIp;
  gsm["net_pin"] = gsmLastNetPin;
  gsm["status_pin"] = gsmLastStatusPin;

  doc["modules"] = pcaCount;
  doc["relays_per"] = RELAYS_PER_MODULE;
  doc["inputs_per"] = INPUTS_PER_MODULE;
  doc["total_relays"] = totalRelays;
  doc["total_inputs"] = totalInputs;
  doc["uptime_ms"] = (uint32_t)millis();
  doc["fw"] = FW_VERSION;
  if(strlen(FW_TAG) > 0) doc["fw_tag"] = FW_TAG;

  if(serializeJson(doc, out) == 0){
    out = "{}";
  }
}

static void sendJsonNetCfg(Client& c){
  // Reload from flash to reflect latest saved mode
  loadNetCfg();
  String out;
  netCfgToJson(out);
  sendText(c, out, "application/json");
}

static void sendJsonWifiCfg(Client& c){
  String out;
  wifiCfgToJson(out);
  sendText(c, out, "application/json");
}

static void sendJsonMqttCfg(Client& c){
  loadMqttCfg();
  mqttSetup();
  String out;
  mqttCfgToJson(out);
  sendText(c, out, "application/json");
}

static void sendJsonBackup(Client& c){
  loadNetCfg();
  loadMqttCfg();

  static JsonDocument doc;
  doc.clear();
  doc["rules"] = rulesDoc;

  JsonObject net = doc["net"].to<JsonObject>();
  net["mode"] = netCfg.dhcp ? "dhcp" : "static";
  net["ip"] = netCfg.ip.toString();
  net["gw"] = netCfg.gw.toString();
  net["sn"] = netCfg.sn.toString();
  net["dns"] = netCfg.dns.toString();

  JsonObject mq = doc["mqtt"].to<JsonObject>();
  mq["enabled"] = mqttCfg.enabled ? 1 : 0;
  mq["transport"] = normalizeMqttTransport(mqttCfg.transport);
  mq["host"] = mqttCfg.host;
  mq["port"] = mqttCfg.port;
  mq["user"] = mqttCfg.user;
  mq["pass"] = mqttCfg.pass;
  mq["gsm_mqtt_host"] = mqttCfg.gsmMqttHost;
  mq["gsm_mqtt_port"] = mqttCfg.gsmMqttPort;
  mq["gsm_mqtt_user"] = mqttCfg.gsmMqttUser;
  mq["gsm_mqtt_pass"] = mqttCfg.gsmMqttPass;
  mq["client_id"] = mqttCfg.clientId;
  mq["base"] = mqttCfg.base;
  mq["discovery_prefix"] = mqttCfg.discoveryPrefix;
  mq["retain"] = mqttCfg.retain ? 1 : 0;
  mq["apn"] = mqttCfg.apn;
  mq["gsm_user"] = mqttCfg.gsmUser;
  mq["gsm_pass"] = mqttCfg.gsmPass;

  String out; serializeJson(doc, out);
  sendText(c, out, "application/json");
}

static bool parseNetFromJson(JsonObject o, NetConfig &nextCfg, String &err) {
  const char* mode = o["mode"] | "static";
  bool dhcp = (strcmp(mode, "dhcp") == 0);
  if(!(dhcp || strcmp(mode, "static")==0)){
    err = "net.mode must be dhcp|static";
    return false;
  }

  nextCfg = netCfg;
  if(!dhcp){
    IPAddress ip, gw, sn, dns;
    bool ok = true;
    const char* ipStr = o["ip"] | "";
    const char* gwStr = o["gw"] | "";
    const char* snStr = o["sn"] | "";
    const char* dnsStr = o["dns"] | "";
    ok &= parseIp(String(ipStr), ip);
    ok &= parseIp(String(gwStr), gw);
    ok &= parseIp(String(snStr), sn);
    ok &= parseIp(String(dnsStr), dns);
    if(!ok){
      err = "net invalid ip fields";
      return false;
    }
    nextCfg.ip = ip;
    nextCfg.gw = gw;
    nextCfg.sn = sn;
    nextCfg.dns = dns;
  }

  nextCfg.dhcp = dhcp;
  return true;
}

static bool applyNetFromJson(JsonObject o, String &err) {
  NetConfig nextCfg;
  if(!parseNetFromJson(o, nextCfg, err)) return false;

  NetConfig prevCfg = netCfg;
  netCfg = nextCfg;
  if(!saveNetCfg()){
    netCfg = prevCfg;
    err = "net fs write failed";
    return false;
  }
  applyNetCfg();
  return true;
}

static bool applyWifiFromJson(JsonObject o, String &err, bool &restarting) {
  restarting = false;
  if(o["enabled"].isNull() && o["pass"].isNull()){
    err = "wifi.enabled or wifi.pass required";
    return false;
  }
  const String oldPass = wifiCfg.pass;
  if(!o["enabled"].isNull()){
    wifiCfg.enabled = (o["enabled"] | 0) ? true : false;
  }
  bool passChanged = false;
  if(!o["pass"].isNull()){
    String p = String((const char*)(o["pass"] | ""));
    if(p.length() < 8){
      err = "wifi.pass must be >= 8 chars";
      return false;
    }
    passChanged = (p != oldPass);
    wifiCfg.pass = p;
  }
  if(!saveWifiCfg()){
    err = "wifi fs write failed";
    return false;
  }
  if(passChanged && wifiCfg.enabled){
    stopWifiAp();
    startWifiAp();
    restarting = true;
  } else {
    applyWifiCfg();
  }
  return true;
}

static bool parseMqttFromJson(JsonObject o, MqttConfig &nextCfg, String &err) {
  int port = (int)(o["port"] | 1883);
  if (port < 1 || port > 65535) {
    err = "mqtt.port out of range";
    return false;
  }
  int gsmMqttPort = (int)(o["gsm_mqtt_port"] | 0);
  if (gsmMqttPort < 0 || gsmMqttPort > 65535) {
    err = "mqtt.gsm_mqtt_port out of range";
    return false;
  }

  nextCfg.enabled = (o["enabled"] | 0) ? true : false;
  nextCfg.transport = normalizeMqttTransport(String((const char*)(o["transport"] | "auto")));
  nextCfg.host = String((const char*)(o["host"] | "192.168.1.43"));
  nextCfg.port = (uint16_t)port;
  nextCfg.user = String((const char*)(o["user"] | ""));
  nextCfg.pass = String((const char*)(o["pass"] | ""));
  nextCfg.gsmMqttHost = String((const char*)(o["gsm_mqtt_host"] | ""));
  nextCfg.gsmMqttPort = (uint16_t)gsmMqttPort;
  nextCfg.gsmMqttUser = String((const char*)(o["gsm_mqtt_user"] | ""));
  nextCfg.gsmMqttPass = String((const char*)(o["gsm_mqtt_pass"] | ""));
  nextCfg.clientId = String((const char*)(o["client_id"] | "ESPRelay4"));
  nextCfg.base = normalizeBaseTopic(String((const char*)(o["base"] | "esprelay4")));
  nextCfg.discoveryPrefix = String((const char*)(o["discovery_prefix"] | "homeassistant"));
  nextCfg.retain = (o["retain"] | 1) ? true : false;
  nextCfg.apn = String((const char*)(o["apn"] | GPRS_DEFAULT_APN));
  nextCfg.gsmUser = String((const char*)(o["gsm_user"] | GPRS_DEFAULT_USER));
  nextCfg.gsmPass = String((const char*)(o["gsm_pass"] | GPRS_DEFAULT_PASS));
  nextCfg.host.trim();
  if (nextCfg.host.length() == 0) {
    err = "mqtt.host required";
    return false;
  }
  nextCfg.gsmMqttHost.trim();
  if (nextCfg.gsmMqttHost.length() > 0 && nextCfg.gsmMqttPort == 0) {
    nextCfg.gsmMqttPort = 1883;
  }
  nextCfg.apn.trim();
  return true;
}

static void mqttOnConfigApplied() {
  gsmMarkDown();
  modemReady = false;
  modemPowerKickDone = false;
  simPinChecked = false;
  modemUartBaud = MODEM_UART_BAUD;
  modemUartRxPin = MODEM_RX;
  modemUartTxPin = MODEM_TX;
  gsmLastTryMs = 0;
  gsmLastDebugMs = 0;
  mqttSetup();
  mqttAnnouncedEth = false;
  mqttAnnouncedGsm = false;
  mqttLastConnectEthMs = 0;
  mqttLastConnectGsmMs = 0;
  lastIpPubEth = "";
  lastIpPubGsm = "";
  mqttLastEthFailMs = 0;
  mqttLastEthFailRc = 0;
  mqttEthAuthBlocked = false;
}

static bool applyMqttFromJson(JsonObject o, String &err) {
  MqttConfig nextCfg;
  if(!parseMqttFromJson(o, nextCfg, err)) return false;

  MqttConfig prevCfg = mqttCfg;
  mqttCfg = nextCfg;
  if(!saveMqttCfg()){
    mqttCfg = prevCfg;
    err = "mqtt fs write failed";
    return false;
  }
  mqttOnConfigApplied();
  return true;
}

static void sendJsonRules(Client& c){
  String out;
  serializeJsonPretty(rulesDoc, out);
  sendText(c, out, "application/json");
}

static bool handleOtaStream(Client& c, int contentLen, bool isFs, const String& expectedSha256, String &err){
  if(contentLen <= 0){ err = "empty body"; return false; }
  if(!Update.begin(contentLen, isFs ? U_SPIFFS : U_FLASH)){
    err = "update begin failed";
    return false;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);

  const int BUF_SIZE = 512;
  uint8_t* buf = (uint8_t*)malloc(BUF_SIZE);
  if(!buf){
    err = "oom";
    Update.abort();
    mbedtls_sha256_free(&ctx);
    return false;
  }
  int remaining = contentLen;
  while(remaining > 0){
    int toRead = remaining > BUF_SIZE ? BUF_SIZE : remaining;
    int n = c.read(buf, toRead);
    if(n <= 0){ delay(1); continue; }
    remaining -= n;
    mbedtls_sha256_update_ret(&ctx, buf, n);
    if(Update.write(buf, n) != (size_t)n){
      free(buf);
      err = "write failed";
      Update.abort();
      mbedtls_sha256_free(&ctx);
      return false;
    }
  }
  free(buf);
  uint8_t hash[32];
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  if(expectedSha256.length() == 64){
    char hex[65];
    for(int i=0;i<32;i++) sprintf(hex + (i*2), "%02x", hash[i]);
    hex[64] = 0;
    String got = String(hex);
    String exp = expectedSha256;
    exp.toLowerCase();
    if(got != exp){
      err = "checksum mismatch";
      Update.abort();
      return false;
    }
  }

  if(!Update.end(true)){
    err = Update.errorString();
    return false;
  }
  return true;
}

static void ethernetPrintInfo() {
  Serial.println("\n[ETH] Ethernet status");
  Serial.printf("  MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print("  IP: "); Serial.println(Ethernet.localIP());
  Serial.print("  GW: "); Serial.println(Ethernet.gatewayIP());
  Serial.print("  MASK: "); Serial.println(Ethernet.subnetMask());
  Serial.print("  DNS: "); Serial.println(Ethernet.dnsServerIP());

  auto hs = Ethernet.hardwareStatus();
  Serial.print("  Hardware: ");
  switch(hs){
    case EthernetNoHardware: Serial.println("No hardware"); break;
    case EthernetW5100:      Serial.println("W5100"); break;
    case EthernetW5200:      Serial.println("W5200"); break;
    case EthernetW5500:      Serial.println("W5500"); break;
    default:                 Serial.println("Unknown"); break;
  }

  auto ls = Ethernet.linkStatus();
  Serial.print("  Link: ");
  switch(ls){
    case LinkON:  Serial.println("ON"); break;
    case LinkOFF: Serial.println("OFF"); break;
    default:      Serial.println("UNKNOWN"); break;
  }
}

static const char* ethernetLinkText(EthernetLinkStatus status) {
  switch (status) {
    case LinkON:  return "ON";
    case LinkOFF: return "OFF";
    default:      return "UNKNOWN";
  }
}

static void logConnectivityTransitions() {
  static bool initDone = false;
  static int8_t lastEth = -2; // -2 uninitialized, -1 unknown, 0 off, 1 on
  static bool lastModem = false;
  static bool lastGsmNet = false;
  static bool lastGsmData = false;

  const EthernetLinkStatus ethLink = Ethernet.linkStatus();
  int8_t ethNow = -1;
  if (ethLink == LinkON) ethNow = 1;
  else if (ethLink == LinkOFF) ethNow = 0;

  if (!initDone || ethNow != lastEth) {
    if (initDone && lastEth == 1 && ethNow != 1) {
      Serial.printf("[ETH] communication perdue (link=%s)\n", ethernetLinkText(ethLink));
    } else if (initDone && lastEth != 1 && ethNow == 1) {
      Serial.println("[ETH] communication retablie (link=ON)");
    } else {
      Serial.printf("[ETH] link=%s\n", ethernetLinkText(ethLink));
    }
    lastEth = ethNow;
  }

  if (!initDone || modemReady != lastModem) {
    Serial.printf("[GSM] etat modem -> %s\n", modemReady ? "READY" : "DOWN");
    lastModem = modemReady;
  }
  if (!initDone || gsmNetworkReady != lastGsmNet) {
    Serial.printf("[GSM] etat reseau -> %s\n", gsmNetworkReady ? "UP" : "DOWN");
    lastGsmNet = gsmNetworkReady;
  }
  if (!initDone || gsmDataReady != lastGsmData) {
    Serial.printf("[GSM] etat data -> %s\n", gsmDataReady ? "UP" : "DOWN");
    lastGsmData = gsmDataReady;
  }

  initDone = true;
}

// ===============================================================
// Validation PUT /api/rules + apply shutter cfg/reservations
// ===============================================================
static bool validateAndApplyRulesDoc(JsonDocument &candidate, String &errMsg) {
  // relays must be array size = totalRelays
  if(!candidate["relays"].is<JsonArray>() || candidate["relays"].as<JsonArray>().size()!=totalRelays){
    errMsg = String("relays must be array size ") + String(totalRelays);
    return false;
  }
  // shutters optional, but if present must be array
  if(!candidate["shutters"].isNull() && !candidate["shutters"].is<JsonArray>()){
    errMsg = "shutters must be array";
    return false;
  }
  if(candidate["shutters"].isNull()){
    candidate["shutters"].to<JsonArray>();
  }
  if(candidate["version"].isNull()) candidate["version"] = 2;

  // parse shutter + validate
  String shErr;
  if(!parseShutterFromRules(candidate["shutters"].as<JsonArray>(), shErr)){
    errMsg = shErr;
    return false;
  }

  // conflicts check (1 volet) already ensures up_relay != down_relay
  // Build reservation map done in parseShutterFromRules

  // IMPORTANT SECURITY: even if relays rules exist for reserved relays, firmware will ignore them.
  // We accept them but they will not be able to drive reserved relays.

  return true;
}

static bool validateRulesDocNoSideEffects(JsonDocument &candidate, String &errMsg) {
  ShutterCfg shCfgBackup[SHUTTER_MAX];
  ShutterRuntime shRtBackup[SHUTTER_MAX];
  bool reservedBackup[MAX_RELAYS];

  for(int i=0; i<SHUTTER_MAX; i++){
    shCfgBackup[i] = shCfg[i];
    shRtBackup[i] = shRt[i];
  }
  for(int i=0; i<MAX_RELAYS; i++){
    reservedBackup[i] = reservedByShutter[i];
  }

  bool ok = validateAndApplyRulesDoc(candidate, errMsg);

  for(int i=0; i<SHUTTER_MAX; i++){
    shCfg[i] = shCfgBackup[i];
    shRt[i] = shRtBackup[i];
  }
  for(int i=0; i<MAX_RELAYS; i++){
    reservedByShutter[i] = reservedBackup[i];
  }
  return ok;
}

static void rebuildRuntimeFromRules() {
  // parse shutter & reservations from current rulesDoc
  String err;
  if(!parseShutterFromRules(rulesDoc["shutters"].as<JsonArray>(), err)){
    // si règles en flash sont invalides, on désactive le volet par sécurité
    Serial.printf("[SHUTTER] invalid rules: %s -> DISABLE shutter\n", err.c_str());
    for(int s=0; s<shuttersLimit(); s++) shCfg[s].enabled = false;
    applyReservationsFromConfig();
  }
  mqttAnnouncedEth = false;
}

// ===============================================================
// HTTP router
// ===============================================================
static void handleHttpClient(Client& client, bool fromWifi=false){
  String req = readLine(client); // "GET /path HTTP/1.1"
  if(req.length()==0){
    client.stop();
    return;
  }

  int contentLen = 0;
  String authHeader = "";
  String contentType = "";
  String checksumSha256 = "";
  while(true){
    String h = readLine(client);
    if(h.length()==0) break;
    if(h.startsWith("Content-Length:")){
      contentLen = h.substring(15).toInt();
    }
    if(h.startsWith("Authorization:")){
      authHeader = h.substring(14);
    }
    if(h.startsWith("Content-Type:")){
      contentType = h.substring(13);
      contentType.trim();
    }
    if(h.startsWith("X-Checksum-Sha256:")){
      checksumSha256 = h.substring(18);
      checksumSha256.trim();
    }
  }

  int sp1 = req.indexOf(' ');
  int sp2 = req.indexOf(' ', sp1+1);
  if (sp1 <= 0 || sp2 <= sp1) {
    sendText(client, "bad request\n", "text/plain", 400);
    delay(1);
    client.stop();
    return;
  }
  String method = req.substring(0, sp1);
  String url = req.substring(sp1+1, sp2);

  String path = url;
  String query = "";
  int q = url.indexOf('?');
  if(q >= 0){ path = url.substring(0,q); query = url.substring(q+1); }
  const bool authed = checkAuthHeader(authHeader);

  // -------- routes --------
  if(method=="GET" && (path=="/" || path=="/index.html")){
    sendFile(client, "/index.html", "text/html; charset=utf-8");
  }
  else if(method=="GET" && (path=="/i18n_en.json" || path=="/i18n_fr.json")){
    sendFile(client, path.c_str(), "application/json; charset=utf-8");
  }
  else if(method=="GET" && path=="/api/state"){
    sendJsonState(client);
  }
  else if(method=="GET" && path=="/api/auth"){
    if(!authed){ sendAuthRequired(client); }
    else {
      String out = String("{\"ok\":true,\"user\":\"") + authCfg.user + "\"}";
      sendText(client, out, "application/json");
    }
  }
  else if(method=="GET" && path=="/api/rules"){
    if(!authed) sendAuthRequired(client);
    else sendJsonRules(client);
  }
  else if(method=="GET" && path=="/api/net"){
    if(!authed) sendAuthRequired(client);
    else sendJsonNetCfg(client);
  }
  else if(method=="GET" && path=="/api/wifi"){
    if(!authed) sendAuthRequired(client);
    else sendJsonWifiCfg(client);
  }
  else if(method=="GET" && path=="/api/mqtt"){
    if(!authed) sendAuthRequired(client);
    else sendJsonMqttCfg(client);
  }
  else if(method=="GET" && path=="/api/backup"){
    if(!authed) sendAuthRequired(client);
    else sendJsonBackup(client);
  }
  else if(method=="PUT" && path=="/api/auth"){
    if(!authed){ sendAuthRequired(client); }
    else {
      String body = readBody(client, contentLen);
      JsonDocument tmp;
      auto err = deserializeJson(tmp, body);
      if(err){
        sendText(client, String("{\"ok\":false,\"error\":\"bad json\"}"), "application/json", 400);
      } else {
        const char* user = tmp["user"] | "";
        const char* pass = tmp["pass"] | "";
        if(String(user).length() == 0 || String(pass).length() == 0){
          sendText(client, String("{\"ok\":false,\"error\":\"user/pass required\"}"), "application/json", 400);
        } else {
          authCfg.user = String(user);
          authCfg.pass = String(pass);
          if(!saveAuthCfg()){
            sendText(client, String("{\"ok\":false,\"error\":\"fs write failed\"}"), "application/json", 500);
          } else {
            sendText(client, String("{\"ok\":true}"), "application/json");
          }
        }
      }
    }
  }
  else if(method=="PUT" && path=="/api/net"){
    if(!authed){ sendAuthRequired(client); return; }
    String body = readBody(client, contentLen);
    JsonDocument tmp;
    auto err = deserializeJson(tmp, body);
    if(err){
      sendText(client, String("{\"ok\":false,\"error\":\"bad json\"}"), "application/json", 400);
    } else {
      const char* mode = tmp["mode"] | "static";
      bool dhcp = (strcmp(mode, "dhcp") == 0);
      if(!(dhcp || strcmp(mode, "static")==0)){
        sendText(client, String("{\"ok\":false,\"error\":\"mode must be dhcp|static\"}"), "application/json", 400);
      } else {
        if(!dhcp){
          IPAddress ip, gw, sn, dns;
          bool ok = true;
          const char* ipStr = tmp["ip"] | "";
          const char* gwStr = tmp["gw"] | "";
          const char* snStr = tmp["sn"] | "";
          const char* dnsStr = tmp["dns"] | "";
          ok &= parseIp(String(ipStr), ip);
          ok &= parseIp(String(gwStr), gw);
          ok &= parseIp(String(snStr), sn);
          ok &= parseIp(String(dnsStr), dns);
          if(!ok){
            sendText(client, String("{\"ok\":false,\"error\":\"invalid ip fields\"}"), "application/json", 400);
            client.stop();
            return;
          }
          netCfg.ip = ip;
          netCfg.gw = gw;
          netCfg.sn = sn;
          netCfg.dns = dns;
        }
        netCfg.dhcp = dhcp;
        if(!saveNetCfg()){
          sendText(client, String("{\"ok\":false,\"error\":\"fs write failed\"}"), "application/json", 500);
        } else {
          sendText(client, String("{\"ok\":true,\"applied\":true}"), "application/json");
          applyNetCfg();
        }
      }
    }
  }
  else if(method=="PUT" && path=="/api/wifi"){
    if(!authed){ sendAuthRequired(client); return; }
    String body = readBody(client, contentLen);
    JsonDocument tmp;
    auto err = deserializeJson(tmp, body);
    if(err){
      sendText(client, String("{\"ok\":false,\"error\":\"bad json\"}"), "application/json", 400);
    } else {
      String errMsg;
      bool restarting = false;
      if(!applyWifiFromJson(tmp.as<JsonObject>(), errMsg, restarting)){
        sendText(client, String("{\"ok\":false,\"error\":\"") + errMsg + "\"}", "application/json", 400);
      } else {
        String out = String("{\"ok\":true,\"applied\":true,\"restarting\":") + (restarting ? "true" : "false") + "}";
        sendText(client, out, "application/json");
      }
    }
  }
  else if(method=="PUT" && path=="/api/mqtt"){
    if(!authed){ sendAuthRequired(client); return; }
    String body = readBody(client, contentLen);
    JsonDocument tmp;
    auto err = deserializeJson(tmp, body);
    if(err){
      sendText(client, String("{\"ok\":false,\"error\":\"bad json\"}"), "application/json", 400);
    } else {
      String errMsg;
      if(!applyMqttFromJson(tmp.as<JsonObject>(), errMsg)){
        sendText(client, String("{\"ok\":false,\"error\":\"") + errMsg + "\"}", "application/json", 400);
      } else {
        sendText(client, String("{\"ok\":true,\"applied\":true}"), "application/json");
      }
    }
  }
  else if(method=="POST" && path=="/api/ota"){
    if(!authed){ sendAuthRequired(client); return; }
    if(contentLen <= 0 || contentType.indexOf("application/octet-stream") < 0){
      sendText(client, String("{\"ok\":false,\"error\":\"octet-stream required\"}"), "application/json", 400);
      client.stop();
      return;
    }
    String errMsg;
    if(!handleOtaStream(client, contentLen, false, checksumSha256, errMsg)){
      sendText(client, String("{\"ok\":false,\"error\":\"") + errMsg + "\"}", "application/json", 400);
    } else {
      sendText(client, String("{\"ok\":true,\"reboot\":true}"), "application/json");
      delay(200);
      ESP.restart();
    }
  }
  else if(method=="POST" && path=="/api/otafs"){
    if(!authed){ sendAuthRequired(client); return; }
    if(contentLen <= 0 || contentType.indexOf("application/octet-stream") < 0){
      sendText(client, String("{\"ok\":false,\"error\":\"octet-stream required\"}"), "application/json", 400);
      client.stop();
      return;
    }
    String errMsg;
    if(!handleOtaStream(client, contentLen, true, checksumSha256, errMsg)){
      sendText(client, String("{\"ok\":false,\"error\":\"") + errMsg + "\"}", "application/json", 400);
    } else {
      sendText(client, String("{\"ok\":true,\"reboot\":true}"), "application/json");
      delay(200);
      ESP.restart();
    }
  }
  else if(method=="PUT" && path=="/api/backup"){
    if(!authed){ sendAuthRequired(client); return; }
    String body = readBody(client, contentLen);
    JsonDocument tmp;
    auto err = deserializeJson(tmp, body);
    if(err){
      sendText(client, String("{\"ok\":false,\"error\":\"bad json\"}"), "application/json", 400);
    } else {
      if(!tmp["rules"].is<JsonObject>() || !tmp["net"].is<JsonObject>() || !tmp["mqtt"].is<JsonObject>()){
        sendText(client, String("{\"ok\":false,\"error\":\"backup must contain rules, net, mqtt\"}"), "application/json", 400);
      } else {
        String msg, errMsg;
        JsonDocument rulesTmp;
        rulesTmp.set(tmp["rules"]);
        NetConfig netNext;
        MqttConfig mqttNext;
        if(!validateRulesDocNoSideEffects(rulesTmp, msg)){
          JsonDocument e;
          e["ok"]=false; e["error"]=msg;
          String out; serializeJson(e,out);
          sendText(client, out, "application/json", 400);
        } else if(!parseNetFromJson(tmp["net"].as<JsonObject>(), netNext, errMsg)){
          sendText(client, String("{\"ok\":false,\"error\":\"") + errMsg + "\"}", "application/json", 400);
        } else if(!parseMqttFromJson(tmp["mqtt"].as<JsonObject>(), mqttNext, errMsg)){
          sendText(client, String("{\"ok\":false,\"error\":\"") + errMsg + "\"}", "application/json", 400);
        } else {
          // Snapshot current state for rollback in case one FS write fails.
          JsonDocument rulesPrev;
          rulesPrev.set(rulesDoc);
          NetConfig netPrev = netCfg;
          MqttConfig mqttPrev = mqttCfg;

          bool ok = true;
          String commitErr;

          rulesDoc.clear();
          rulesDoc.set(rulesTmp);
          rebuildRuntimeFromRules();
          if(!saveRulesToFS(rulesDoc)){
            ok = false;
            commitErr = "rules fs write failed";
          }

          if(ok){
            netCfg = netNext;
            if(!saveNetCfg()){
              ok = false;
              commitErr = "net fs write failed";
            } else {
              applyNetCfg();
            }
          }

          if(ok){
            mqttCfg = mqttNext;
            if(!saveMqttCfg()){
              ok = false;
              commitErr = "mqtt fs write failed";
            } else {
              mqttOnConfigApplied();
            }
          }

          if(!ok){
            // Rollback all configs to keep backup restore coherent.
            rulesDoc.clear();
            rulesDoc.set(rulesPrev);
            saveRulesToFS(rulesDoc);
            rebuildRuntimeFromRules();

            netCfg = netPrev;
            saveNetCfg();
            applyNetCfg();

            mqttCfg = mqttPrev;
            saveMqttCfg();
            mqttOnConfigApplied();

            sendText(client, String("{\"ok\":false,\"error\":\"") + commitErr + "\"}", "application/json", 500);
          } else {
            sendText(client, String("{\"ok\":true,\"applied\":true,\"reboot\":true}"), "application/json");
            delay(200);
            ESP.restart();
          }
        }
      }
    }
  }
  else if(method=="PUT" && path=="/api/rules"){
    if(!authed){ sendAuthRequired(client); return; }
    String body = readBody(client, contentLen);

    JsonDocument tmp;
    auto err = deserializeJson(tmp, body);
    if(err){
      sendText(client, String("{\"ok\":false,\"error\":\"bad json\"}"), "application/json", 400);
    } else {
      String msg;
      if(!validateRulesDocNoSideEffects(tmp, msg)){
        JsonDocument e;
        e["ok"]=false; e["error"]=msg;
        String out; serializeJson(e,out);
        sendText(client, out, "application/json", 400);
      } else {
        // appliquer + sauver
        rulesDoc.clear();
        rulesDoc.set(tmp);
        rebuildRuntimeFromRules();

        if(!saveRulesToFS(rulesDoc)){
          sendText(client, String("{\"ok\":false,\"error\":\"fs write failed\"}"), "application/json", 500);
        } else {
          sendText(client, String("{\"ok\":true,\"applied\":true}"), "application/json");
        }
      }
    }
  }
  else if(method=="POST" && path=="/api/override"){
    if(!authed){ sendAuthRequired(client); return; }
    // Strict protection: refuse override on reserved relays
    String body = readBody(client, contentLen);
    JsonDocument doc;
    auto err = deserializeJson(doc, body);
    if(err){
      sendText(client, String("{\"ok\":false,\"error\":\"bad json\"}"), "application/json", 400);
    } else {
      int r = doc["relay"] | 1; // 1..totalRelays
      const char* mode = doc["mode"] | "AUTO";
      if(r < 1 || r > totalRelays){
        sendText(client, String("{\"ok\":false,\"error\":\"relay out of range\"}"), "application/json", 400);
      } else {
        int idx = r-1;
        if(reservedByShutter[idx]){
          sendText(client, String("{\"ok\":false,\"error\":\"relay reserved by shutter\"}"), "application/json", 400);
        } else {
          if(strcmp(mode,"AUTO")==0) overrideRelay[idx] = -1;
          else if(strcmp(mode,"FORCE_ON")==0) overrideRelay[idx] = 1;
          else if(strcmp(mode,"FORCE_OFF")==0) overrideRelay[idx] = 0;
          else {
            sendText(client, String("{\"ok\":false,\"error\":\"mode must be AUTO|FORCE_ON|FORCE_OFF\"}"), "application/json", 400);
            client.stop();
            return;
          }
          sendText(client, String("{\"ok\":true}"), "application/json");
        }
      }
    }
  }
  else if(method=="POST" && path=="/api/shutter"){
    if(!authed){ sendAuthRequired(client); return; }
    // Commande volet: { "id":1|2, "cmd":"UP|DOWN|STOP|AUTO" }
    String body = readBody(client, contentLen);
    JsonDocument doc;
    auto err = deserializeJson(doc, body);
    if(err){
      sendText(client, String("{\"ok\":false,\"error\":\"bad json\"}"), "application/json", 400);
    } else {
      const char* cmd = doc["cmd"] | "STOP";
      int sid = doc["id"] | 1;
      if(sid < 1 || sid > shuttersLimit()){
        sendText(client, String("{\"ok\":false,\"error\":\"id out of range\"}"), "application/json", 400);
      } else if(!shCfg[sid-1].enabled){
        sendText(client, String("{\"ok\":false,\"error\":\"no shutter configured\"}"), "application/json", 400);
      } else if(strcmp(cmd,"UP")==0){
        shRt[sid-1].manual = MC_UP;
        sendText(client, String("{\"ok\":true}"), "application/json");
      } else if(strcmp(cmd,"DOWN")==0){
        shRt[sid-1].manual = MC_DOWN;
        sendText(client, String("{\"ok\":true}"), "application/json");
      } else if(strcmp(cmd,"STOP")==0){
        shRt[sid-1].manual = MC_STOP;
        sendText(client, String("{\"ok\":true}"), "application/json");
      } else if(strcmp(cmd,"AUTO")==0){
        // option: rendre la main aux boutons (désactive le manuel)
        shRt[sid-1].manual = MC_NONE;
        sendText(client, String("{\"ok\":true}"), "application/json");
      } else {
        sendText(client, String("{\"ok\":false,\"error\":\"cmd must be UP|DOWN|STOP|AUTO\"}"), "application/json", 400);
      }
    }
  }
  else if(method=="GET" && fromWifi && wifiApOn && (
      path=="/generate_204" || path=="/gen_204" ||
      path=="/hotspot-detect.html" || path=="/success.txt" ||
      path=="/ncsi.txt" || path=="/connecttest.txt"
    )){
    client.println("HTTP/1.1 302 Found");
    client.print("Location: http://"); client.print(WiFi.softAPIP().toString()); client.println("/");
    client.println("Cache-Control: no-cache");
    client.println("Connection: close");
    client.println();
  }
  else{
    if(fromWifi && wifiApOn && method=="GET" && !path.startsWith("/api") && path != "/i18n_en.json" && path != "/i18n_fr.json"){
      client.println("HTTP/1.1 302 Found");
      client.print("Location: http://"); client.print(WiFi.softAPIP().toString()); client.println("/");
      client.println("Cache-Control: no-cache");
      client.println("Connection: close");
      client.println();
    } else {
      sendText(client, "not found\n", "text/plain", 404);
    }
  }

  delay(5);
  client.stop();
}

static void handleHttp(){
  EthernetClient ethClient = server.available();
  if(ethClient) {
    handleHttpClient(ethClient, false);
  }

  if(wifiApOn){
    wifiDns.processNextRequest();
    WiFiClient wifiClient = wifiServer.available();
    if(wifiClient) {
      handleHttpClient(wifiClient, true);
    }
  }
}

// ===============================================================
// Setup / Loop
// ===============================================================
void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, 0);

  Serial.begin(115200);
  delay(600);
  Serial.println("\n=== BOOT Automate PCA9538 + W5500 + Rules + Shutter ownership ===");
  Serial.printf("[MODEM] EN=%d PWRKEY=%d NET=%d RX=%d TX=%d\n",
                PIN_MODEM_EN, PIN_MODEM_PWRKEY, PIN_MODEM_NET, PIN_MODEM_RX, PIN_MODEM_TX);
  Serial.printf("[GSM][1NCE] defaults apn=%s sim_pin=%s\n",
                GPRS_DEFAULT_APN, (strlen(SIM_PIN) > 0 ? "set" : "empty"));

  modemDriveExpectedPins();
  if (PIN_MODEM_STATUS >= 0) pinMode(PIN_MODEM_STATUS, INPUT);
  if (PIN_MODEM_NET >= 0) pinMode(PIN_MODEM_NET, INPUT);
  delay(2);
  modemVerifyStartupPins();

  // LittleFS
  if(!LittleFS.begin(true)){
    Serial.println("[FS] LittleFS init FAILED");
  } else {
    Serial.println("[FS] LittleFS OK");
    File f = LittleFS.open("/index.html", "r");
    if(f){ Serial.printf("[FS] /index.html size=%u bytes\n", (unsigned)f.size()); f.close(); }
    else Serial.println("[FS] /index.html NOT found (run uploadfs)");
  }
  logFactoryPinState();
  if(factoryResetHeld()) doFactoryReset();
  loadAuthCfg();

  // I2C
  Serial.printf("[I2C] SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(20); // avoid long blocking I2C calls that can starve HTTP loop

  // 1-Wire temp sensors
  tempSensors.begin();
  tempSensors.setWaitForConversion(false); // avoid blocking up to ~750ms per request
  tempCount = tempSensors.getDeviceCount();
  if(tempCount > TEMP_MAX_SENSORS) tempCount = TEMP_MAX_SENSORS;
  for (uint8_t i = 0; i < tempCount; i++) {
    if(tempSensors.getAddress(tempAddr[i], i)) {
      tempC[i] = -127.0f;
      lastTempPub[i] = -127.0f;
    }
  }
  Serial.printf("[TEMP] sensors=%u on GPIO%d\n", tempCount, PIN_ONEWIRE);

  // DHT22
  dht.begin();

  // PCA9538 (scan 0x70..0x73)
  Serial.printf("[PCA9538] scan 0x%02X..0x%02X\n", PCA_BASE_ADDR, PCA_BASE_ADDR + PCA_MAX_MODULES - 1);
  pcaScanAndInit();
  Serial.printf("[PCA9538] modules found=%u (relays=%u inputs=%u)\n", pcaCount, totalRelays, totalInputs);

  // Rules
  loadRulesFromFS();
  rebuildRuntimeFromRules();

  // Ethernet
  loadNetCfg();
  loadWifiCfg();
  loadBleCfg();
  buildEthernetMac();
  applyNetCfg();
  ethernetPrintInfo();
  applyWifiCfg();
  initBle();

  // MQTT
  loadMqttCfg();
  String mqttBootTransport = normalizeMqttTransport(mqttCfg.transport);
  Serial.printf("[MQTT] cfg enabled=%d transport=%s host=%s:%u gsm_host=%s:%u\n",
                mqttCfg.enabled ? 1 : 0,
                mqttBootTransport.c_str(),
                mqttCfg.host.c_str(),
                mqttCfg.port,
                mqttCfg.gsmMqttHost.length() ? mqttCfg.gsmMqttHost.c_str() : "-",
                mqttCfg.gsmMqttPort);
  if (mqttTransportAllowsGsm()) {
    gsmLastTryMs = 0; // force immediate first attempt
    if (gsmEnsureData()) {
      Serial.println("[GSM] startup 1NCE: connected");
    } else {
      Serial.println("[GSM] startup 1NCE: failed");
    }
  } else {
    Serial.println("[GSM] startup skipped (transport mode)");
  }
  mqttSetup();

  digitalWrite(PIN_LED, 1);
  Serial.println("[BOOT] Ready. Open http://<IP>/");
}

void loop() {
  // Serve HTTP first to keep UI/API responsive even if other tasks slow down.
  handleHttp();

  // Process MQTT early so incoming commands are applied in the same loop cycle.
  mqttLoop();
  if (mqttFastCommandPending) {
    // Fast path: apply command immediately, then continue normal cycle.
    shutterTick();
    evalSimpleRules();
    buildFinalRelays();
    pcaApplyRelays();
    mqttFastCommandPending = false;
  }

  // read inputs early to update module status before serving HTTP
  pcaReadInputs();
  debounceInputs();

  // combine physical + virtual inputs for rules/edges
  for(int i=0;i<totalInputs;i++){
    combinedInputs[i] = inputs[i] || virtualInputs[i];
  }

  updateWifiState();
  heartbeatTick();
  bleTick();

  // tick shutter BEFORE computing simple rules, so it can use prevInputs if needed
  shutterTick();

  // compute simple rules (for all relays)
  evalSimpleRules();

  // build final outputs with ownership rules:
  // simple -> shutter overwrites reserved -> overrides (non-reserved only) -> final safety
  buildFinalRelays();

  // apply outputs
  pcaApplyRelays();

  // Temperature polling
  if(millis() - lastTempReadMs > 5000){
    lastTempReadMs = millis();
    if(tempCount > 0){
      tempSensors.requestTemperatures();
      for(uint8_t i=0;i<tempCount;i++){
        float c = tempSensors.getTempC(tempAddr[i]);
        tempC[i] = c;
      }
    }
    // DHT reads can block when sensor is absent; once not detected, probe only occasionally.
    const uint32_t nowMs = millis();
    const bool shouldProbeDht = dhtPresent || !dhtCheckDone || nowMs >= dhtNextProbeMs;
    if (shouldProbeDht) {
      float dhtC = dht.readTemperature();
      float dhtH = dht.readHumidity();
      if(!isnan(dhtC) || !isnan(dhtH)){
        if(!dhtPresent) mqttAnnouncedEth = false;
        if(!dhtPresent) Serial.println("[DHT] detected");
        dhtPresent = true;
        if(!isnan(dhtC)) dhtTempC = dhtC;
        if(!isnan(dhtH)) dhtHum = dhtH;
        dhtCheckDone = true;
        dhtNextProbeMs = nowMs + 5000;
      } else {
        if(!dhtCheckDone) {
          Serial.println("[DHT] not detected");
          dhtCheckDone = true;
        }
        if (!dhtPresent) {
          dhtNextProbeMs = nowMs + 60000;
        }
      }
    }
  }

  // update prev inputs for edge-based rules/toggle/pulse
  for(int k=0;k<totalInputs;k++){
    prevInputs[k] = inputs[k];
    prevCombinedInputs[k] = combinedInputs[k];
  }

  // 1Hz log
  /*
  static uint32_t t0 = 0;
  if(millis() - t0 > 1000){
    t0 = millis();
    logConnectivityTransitions();
    String e, r, res;
    for(int i=0;i<totalInputs;i++) e += String(inputs[i] ? 1 : 0);
    for(int i=0;i<totalRelays;i++) r += String(relays[i] ? 1 : 0);
    for(int i=0;i<totalRelays;i++) res += String(reservedByShutter[i] ? 1 : 0);
    Serial.printf("[STATE] E=%s  R=%s  RES=%s\n",
      e.c_str(), r.c_str(), res.c_str()
    );
  }
  */

  delay(2);
}
