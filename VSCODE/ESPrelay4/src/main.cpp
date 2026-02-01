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
//   POST /api/ota          -> update firmware (multipart/form-data)
//   POST /api/otafs        -> update LittleFS (multipart/form-data)
//   POST /api/override     -> override d'un relais (REFUSE si relais réservé volet)
//   POST /api/shutter      -> commande volet (UP/DOWN/STOP) (seul moyen "API" de bouger les relais volet)


#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <Update.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>

// ===================== A ADAPTER A TON PCB =====================
static const uint8_t PIN_LED = 40;
static const uint8_t PIN_ONEWIRE = 1;  // DS18B20 (IO1)
static const uint8_t PIN_DHT = 2;      // DHT22 (IO2)
static const uint8_t PIN_FACTORY = 0;  // IO0 factory reset button (hold 10s at boot)

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
  String host;
  uint16_t port;
  String user;
  String pass;
  String clientId;
  String base;
  String discoveryPrefix;
  bool retain;
};

static MqttConfig mqttCfg = {
  false,
  String("192.168.1.43"),
  1883,
  String(""),
  String(""),
  String("ESPRelay4"),
  String("esprelay4"),
  String("homeassistant"),
  true
};

static EthernetClient mqttEth;
static PubSubClient mqttClient(mqttEth);
static uint32_t mqttLastConnectMs = 0;
static bool mqttAnnounced = false;
static bool lastInputsPub[MAX_INPUTS] = {false};
static bool lastRelaysPub[MAX_RELAYS] = {false};
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


struct AuthConfig {
  String user;
  String pass;
};

static AuthConfig authCfg = {"admin", "admin"};


// ===================== Etat IO =====================
bool inputs[MAX_INPUTS] = {0};
bool prevInputs[MAX_INPUTS] = {0};

bool relays[MAX_RELAYS] = {0};
bool relayFromSimple[MAX_RELAYS] = {0};
bool relayFromShutter[MAX_RELAYS] = {0};

uint8_t pcaOutCache[PCA_MAX_MODULES] = {0};
bool pcaPresent[PCA_MAX_MODULES] = {false};
uint8_t pcaCount = 0;
uint8_t totalRelays = 4;
uint8_t totalInputs = 4;

// Overrides : -1 auto, 0 force off, 1 force on (uniquement pour relais NON réservés)
int8_t overrideRelay[MAX_RELAYS];

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
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false; // STOP
  if (Wire.requestFrom((int)addr, 1) != 1) return false;
  val = Wire.read();
  return true;
}

static bool i2cWriteReg8(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission(true) == 0; // STOP
}

// ===============================================================
// LittleFS helpers
// ===============================================================
static String readFile(const char* path) {
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
  JsonDocument doc;
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
  JsonDocument doc;
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
    wifiApOn = true;
    Serial.printf("[WIFI] AP ON SSID=%s PASS=%s IP=%s\n", wifiCfg.ssid.c_str(), wifiCfg.pass.c_str(), WiFi.softAPIP().toString().c_str());
  } else {
    wifiApOn = false;
    Serial.println("[WIFI] AP start FAILED");
  }
}

static void stopWifiAp(){
  if(!wifiApOn) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiApOn = false;
  Serial.println("[WIFI] AP OFF");
}

static void updateWifiState(bool force=false){
  uint32_t now = millis();
  if(!force && (now - wifiLastCheckMs < 1000)) return;
  wifiLastCheckMs = now;
  const bool shouldRun = wifiCfg.enabled;
  if(shouldRun && !wifiApOn) startWifiAp();
  else if(!shouldRun && wifiApOn) stopWifiAp();
}

static void applyWifiCfg(){
  updateWifiState(true);
}

// Auth config (LittleFS)
static void authCfgToJson(String &out){
  JsonDocument doc;
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
  JsonDocument doc;
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
    if(now - lastBlink >= 250){
      lastBlink = now;
      ledOn = !ledOn;
      digitalWrite(PIN_LED, ledOn ? 1 : 0);
    }
    delay(20);
  }
  digitalWrite(PIN_LED, 1);
  return true;
}

static void doFactoryReset(){
  Serial.println("[FACTORY] button held 10s -> reset config");
  const char* files[] = {"/net.json", "/mqtt.json", "/rules.json", "/auth.json", "/wifi.json"};
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
  JsonDocument doc;
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
  JsonDocument doc;
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
    client.println("HTTP/1.1 404 Not Found");
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.print("File not found: ");
    client.println(path);
    return;
  }

  size_t size = f.size();
  client.println("HTTP/1.1 200 OK");
  client.print("Content-Type: "); client.println(contentType);
  client.print("Content-Length: "); client.println(size);
  client.println("Connection: close");
  client.println();

  uint8_t buf[1024];
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n > 0) client.write(buf, n);
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

  if (!i2cWriteReg8(addr, REG_POL, 0x00)) return false;
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
    if (pcaInitModule(addr, pcaOutCache[m])) {
      pcaPresent[m] = true;
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
  }
}

static void pcaReadInputs() {
  for (uint8_t m = 0; m < PCA_MAX_MODULES; m++) {
    uint8_t base = m * INPUTS_PER_MODULE;
    if (!pcaPresent[m]) {
      for (uint8_t i = 0; i < INPUTS_PER_MODULE; i++) inputs[base + i] = false;
      continue;
    }
    uint8_t in = 0;
    if(!i2cReadReg8(PCA_BASE_ADDR + m, REG_INPUT, in)) continue;
    for(uint8_t i=0;i<INPUTS_PER_MODULE;i++){
      inputs[base + i] = (in >> (4+i)) & 0x1; // IO4..IO7
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
  expr["op"] = "FOLLOW";
  expr["in"] = (i+1 <= totalInputs) ? (i+1) : 1;
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

static void mqttCfgToJson(String &out) {
  JsonDocument doc;
  doc["enabled"] = mqttCfg.enabled ? 1 : 0;
  doc["host"] = mqttCfg.host;
  doc["port"] = mqttCfg.port;
  doc["user"] = mqttCfg.user;
  doc["pass"] = mqttCfg.pass;
  doc["client_id"] = mqttCfg.clientId;
  doc["base"] = mqttCfg.base;
  doc["discovery_prefix"] = mqttCfg.discoveryPrefix;
  doc["retain"] = mqttCfg.retain ? 1 : 0;
  doc["connected"] = mqttClient.connected() ? 1 : 0;
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
    saveMqttCfg();
    Serial.println("[MQTT] created default /mqtt.json");
    return true;
  }
  JsonDocument doc;
  auto err = deserializeJson(doc, s);
  if (err) {
    Serial.printf("[MQTT] JSON parse error -> keep default (%s)\n", err.c_str());
    return false;
  }
  mqttCfg.enabled = (doc["enabled"] | 0) ? true : false;
  mqttCfg.host = String((const char*)(doc["host"] | "192.168.1.43"));
  mqttCfg.port = (uint16_t)(doc["port"] | 1883);
  mqttCfg.user = String((const char*)(doc["user"] | ""));
  mqttCfg.pass = String((const char*)(doc["pass"] | ""));
  mqttCfg.clientId = String((const char*)(doc["client_id"] | "ESPRelay4"));
  mqttCfg.base = normalizeBaseTopic(String((const char*)(doc["base"] | "esprelay4")));
  mqttCfg.discoveryPrefix = String((const char*)(doc["discovery_prefix"] | "homeassistant"));
  mqttCfg.retain = (doc["retain"] | 1) ? true : false;
  return true;
}

static void mqttPublish(const String& topic, const String& payload, bool retain) {
  mqttClient.publish(topic.c_str(), payload.c_str(), retain);
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

static void mqttPublishDiscovery() {
  if (!mqttClient.connected()) return;
  String base = mqttBaseTopic();
  String node = mqttNodeId();
  String avail = base + "/status";
  String id = node;

  for (int i = 0; i < totalRelays; i++) {
    JsonDocument doc;
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
    mqttPublish(topic, out, true);
  }

  for (int i = 0; i < totalInputs; i++) {
    JsonDocument doc;
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
    mqttPublish(topic, out, true);
  }

  for (int s = 0; s < shuttersLimit(); s++) {
    if (!shCfg[s].enabled) continue;
    JsonDocument doc;
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
    mqttPublish(topic, out, true);
  }

  // Temperature sensors
  for (int i = 0; i < tempCount; i++) {
    JsonDocument doc;
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
    mqttPublish(topic, out, true);
  }

  if (dhtPresent) {
    JsonDocument doc;
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
    mqttPublish(topic, out, true);
  }
  if (dhtPresent) {
    JsonDocument doc;
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
    mqttPublish(topic, out, true);
  }

  mqttAnnounced = true;
}

static void mqttPublishAllState() {
  if (!mqttClient.connected()) return;
  String base = mqttBaseTopic();
  mqttPublish(base + "/status", "online", true);
  for (int i = 0; i < totalRelays; i++) {
    mqttPublish(base + "/relay/" + String(i+1) + "/state", relays[i] ? "ON" : "OFF", mqttCfg.retain);
    lastRelaysPub[i] = relays[i];
  }
  for (int i = 0; i < totalInputs; i++) {
    mqttPublish(base + "/input/" + String(i+1) + "/state", inputs[i] ? "ON" : "OFF", mqttCfg.retain);
    lastInputsPub[i] = inputs[i];
  }
  for (int s = 0; s < shuttersLimit(); s++) {
    if (!shCfg[s].enabled) continue;
    const char* st = (shRt[s].move==SH_UP ? "opening" : (shRt[s].move==SH_DOWN ? "closing" : "stopped"));
    mqttPublish(base + "/shutter/" + String(s+1) + "/state", String(st), mqttCfg.retain);
    lastShutterMove[s] = (int)shRt[s].move;
  }
  for (int i = 0; i < tempCount; i++) {
    if (tempC[i] > -100.0f) {
      mqttPublish(base + "/temp/" + String(i+1) + "/state", String(tempC[i], 2), mqttCfg.retain);
      lastTempPub[i] = tempC[i];
    }
  }
  if (dhtPresent && !isnan(dhtTempC)) {
    mqttPublish(base + "/temp/dht/state", String(dhtTempC, 2), mqttCfg.retain);
    lastDhtPub = dhtTempC;
  }
  if (dhtPresent && !isnan(dhtHum)) {
    mqttPublish(base + "/hum/dht/state", String(dhtHum, 1), mqttCfg.retain);
    lastDhtHumPub = dhtHum;
  }
}

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String p;
  for (unsigned int i = 0; i < length; i++) p += (char)payload[i];
  p.trim();
  p.toUpperCase();
  Serial.printf("[MQTT] RX topic=%s payload=%s\n", t.c_str(), p.c_str());

  String base = mqttBaseTopic();
  if (t.startsWith(base + "/relay/") && t.endsWith("/set")) {
    int idx = t.substring((base + "/relay/").length()).toInt();
    if (idx >= 1 && idx <= totalRelays) {
      int i = idx - 1;
      if (!reservedByShutter[i]) {
        if (p == "ON") overrideRelay[i] = 1;
        else if (p == "OFF") overrideRelay[i] = 0;
        else if (p == "AUTO") overrideRelay[i] = -1;
        else if (p == "TOGGLE") overrideRelay[i] = (overrideRelay[i] == 1 ? 0 : 1);
      }
    }
  } else if (t.startsWith(base + "/shutter/") && t.endsWith("/set")) {
    int idx = t.substring((base + "/shutter/").length()).toInt();
    if (idx >= 1 && idx <= shuttersLimit()) {
      int s = idx - 1;
      if (p == "OPEN" || p == "UP") shRt[s].manual = MC_UP;
      else if (p == "CLOSE" || p == "DOWN") shRt[s].manual = MC_DOWN;
      else if (p == "STOP") shRt[s].manual = MC_STOP;
    }
  }
}

static void mqttSetup() {
  mqttCfg.base = normalizeBaseTopic(mqttCfg.base);
  mqttClient.setServer(mqttCfg.host.c_str(), mqttCfg.port);
  mqttClient.setCallback(mqttCallback);
}

static void mqttSubscribeTopics() {
  String base = mqttBaseTopic();
  for (int i = 1; i <= totalRelays; i++) {
    mqttClient.subscribe((base + "/relay/" + String(i) + "/set").c_str());
  }
  for (int s = 1; s <= shuttersLimit(); s++) {
    mqttClient.subscribe((base + "/shutter/" + String(s) + "/set").c_str());
  }
}

static void mqttEnsureConnected() {
  if (!mqttCfg.enabled) return;
  if (mqttClient.connected()) return;
  uint32_t now = millis();
  if (now - mqttLastConnectMs < 3000) return;
  mqttLastConnectMs = now;

  String clientId = mqttNodeId();
  String willTopic = mqttBaseTopic() + "/status";
  bool ok = false;
  Serial.printf("[MQTT] connect %s:%u client=%s\n", mqttCfg.host.c_str(), mqttCfg.port, clientId.c_str());
  if (mqttCfg.user.length() > 0) {
    ok = mqttClient.connect(clientId.c_str(), mqttCfg.user.c_str(), mqttCfg.pass.c_str(),
                            willTopic.c_str(), 0, true, "offline");
  } else {
    ok = mqttClient.connect(clientId.c_str(), willTopic.c_str(), 0, true, "offline");
  }
  if (ok) {
    Serial.println("[MQTT] connected");
    mqttSubscribeTopics();
    mqttPublishAllState();
    mqttPublishDiscovery();
  } else {
    Serial.printf("[MQTT] connect failed rc=%d\n", mqttClient.state());
  }
}

static void mqttLoop() {
  if (!mqttCfg.enabled) return;
  mqttEnsureConnected();
  mqttClient.loop();

  if (!mqttClient.connected()) return;

  if (!mqttAnnounced) {
    mqttPublishDiscovery();
  }

  String base = mqttBaseTopic();
  for (int i = 0; i < totalInputs; i++) {
    if (inputs[i] != lastInputsPub[i]) {
      mqttPublish(base + "/input/" + String(i+1) + "/state", inputs[i] ? "ON" : "OFF", mqttCfg.retain);
      lastInputsPub[i] = inputs[i];
    }
  }
  for (int i = 0; i < totalRelays; i++) {
    if (relays[i] != lastRelaysPub[i]) {
      mqttPublish(base + "/relay/" + String(i+1) + "/state", relays[i] ? "ON" : "OFF", mqttCfg.retain);
      lastRelaysPub[i] = relays[i];
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

  for (int i = 0; i < tempCount; i++) {
    if (fabs(tempC[i] - lastTempPub[i]) >= 0.1f) {
      mqttPublish(base + "/temp/" + String(i+1) + "/state", String(tempC[i], 2), mqttCfg.retain);
      lastTempPub[i] = tempC[i];
    }
  }

  if (dhtPresent && !isnan(dhtTempC)) {
    if (isnan(lastDhtPub) || fabs(dhtTempC - lastDhtPub) >= 0.1f) {
      mqttPublish(base + "/temp/dht/state", String(dhtTempC, 2), mqttCfg.retain);
      lastDhtPub = dhtTempC;
    }
  }
  if (dhtPresent && !isnan(dhtHum)) {
    if (isnan(lastDhtHumPub) || fabs(dhtHum - lastDhtHumPub) >= 0.5f) {
      mqttPublish(base + "/hum/dht/state", String(dhtHum, 1), mqttCfg.retain);
      lastDhtHumPub = dhtHum;
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
  return inputs[n-1];
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

static bool evalExprSimple(int relayIndex, JsonObject expr) {
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
      thisRise = (inputs[in-1] && !prevInputs[in-1]);
    }
    if(thisRise) toggleState[relayIndex] = !toggleState[relayIndex];
    return toggleState[relayIndex];
  }
  if(strcmp(op,"PULSE_RISE")==0){
    int in = expr["in"] | 1;
    uint32_t pulseMs = expr["pulseMs"] | 200;
    bool thisRise = false;
    if(in>=1 && in<=totalInputs){
      thisRise = (inputs[in-1] && !prevInputs[in-1]);
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
      desired = evalExprSimple(i, expr);

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
  String s;
  while(c.connected()){
    if(c.available()){
      char ch = c.read();
      if(ch=='\n') break;
      if(ch!='\r') s += ch;
    }
  }
  return s;
}

static void sendText(Client& c, const String& body, const char* ctype, int code);

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
  String b; b.reserve(len);
  while(len-- > 0){
    while(!c.available()) delay(1);
    b += (char)c.read();
  }
  return b;
}

static void sendText(Client& c, const String& body, const char* ctype, int code=200){
  if(code==200) c.println("HTTP/1.1 200 OK");
  else if(code==401) c.println("HTTP/1.1 401 Unauthorized");
  else if(code==204) c.println("HTTP/1.1 204 No Content");
  else if(code==400) c.println("HTTP/1.1 400 Bad Request");
  else if(code==404) c.println("HTTP/1.1 404 Not Found");
  else c.println("HTTP/1.1 500 Internal Server Error");

  c.print("Content-Type: "); c.println(ctype);
  c.println("Connection: close");
  c.print("Content-Length: "); c.println(body.length());
  c.println();
  c.print(body);
}

static void sendJsonState(Client& c){
  JsonDocument doc;
  JsonArray inA = doc["inputs"].to<JsonArray>();
  JsonArray reA = doc["relays"].to<JsonArray>();
  JsonArray ovA = doc["override"].to<JsonArray>();
  JsonArray rsA = doc["reserved"].to<JsonArray>();

  for(int i=0;i<totalInputs;i++){
    inA.add(inputs[i] ? 1 : 0);
  }
  for(int i=0;i<totalRelays;i++){
    reA.add(relays[i] ? 1 : 0);
    ovA.add(overrideRelay[i]);
    rsA.add(reservedByShutter[i] ? 1 : 0);
  }

  JsonObject eth = doc["eth"].to<JsonObject>();
  eth["link"] = (Ethernet.linkStatus()==LinkON) ? 1 : 0;
  eth["ip"] = Ethernet.localIP().toString();

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["enabled"] = wifiCfg.enabled ? 1 : 0;
  wifi["ap"] = wifiApOn ? 1 : 0;
  wifi["ssid"] = wifiCfg.ssid;
  wifi["pass"] = wifiCfg.pass;
  wifi["ip"] = wifiApOn ? WiFi.softAPIP().toString() : "";

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
  doc["uptime_ms"] = (uint32_t)millis();

  String out; serializeJson(doc, out);
  sendText(c, out, "application/json");
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

  JsonDocument doc;
  doc["rules"] = rulesDoc;

  JsonObject net = doc["net"].to<JsonObject>();
  net["mode"] = netCfg.dhcp ? "dhcp" : "static";
  net["ip"] = netCfg.ip.toString();
  net["gw"] = netCfg.gw.toString();
  net["sn"] = netCfg.sn.toString();
  net["dns"] = netCfg.dns.toString();

  JsonObject mq = doc["mqtt"].to<JsonObject>();
  mq["enabled"] = mqttCfg.enabled ? 1 : 0;
  mq["host"] = mqttCfg.host;
  mq["port"] = mqttCfg.port;
  mq["user"] = mqttCfg.user;
  mq["pass"] = mqttCfg.pass;
  mq["client_id"] = mqttCfg.clientId;
  mq["base"] = mqttCfg.base;
  mq["discovery_prefix"] = mqttCfg.discoveryPrefix;
  mq["retain"] = mqttCfg.retain ? 1 : 0;

  String out; serializeJson(doc, out);
  sendText(c, out, "application/json");
}

static bool applyNetFromJson(JsonObject o, String &err) {
  const char* mode = o["mode"] | "static";
  bool dhcp = (strcmp(mode, "dhcp") == 0);
  if(!(dhcp || strcmp(mode, "static")==0)){
    err = "net.mode must be dhcp|static";
    return false;
  }

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
    netCfg.ip = ip;
    netCfg.gw = gw;
    netCfg.sn = sn;
    netCfg.dns = dns;
  }

  netCfg.dhcp = dhcp;
  if(!saveNetCfg()){
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

static bool applyMqttFromJson(JsonObject o, String &err) {
  mqttCfg.enabled = (o["enabled"] | 0) ? true : false;
  mqttCfg.host = String((const char*)(o["host"] | "192.168.1.43"));
  mqttCfg.port = (uint16_t)(o["port"] | 1883);
  mqttCfg.user = String((const char*)(o["user"] | ""));
  mqttCfg.pass = String((const char*)(o["pass"] | ""));
  mqttCfg.clientId = String((const char*)(o["client_id"] | "ESPRelay4"));
  mqttCfg.base = normalizeBaseTopic(String((const char*)(o["base"] | "esprelay4")));
  mqttCfg.discoveryPrefix = String((const char*)(o["discovery_prefix"] | "homeassistant"));
  mqttCfg.retain = (o["retain"] | 1) ? true : false;

  if(!saveMqttCfg()){
    err = "mqtt fs write failed";
    return false;
  }
  mqttSetup();
  mqttAnnounced = false;
  return true;
}

static void sendJsonRules(Client& c){
  String out;
  serializeJsonPretty(rulesDoc, out);
  sendText(c, out, "application/json");
}

static bool readLineBody(Client& c, int &remaining, String &out){
  out = "";
  while(remaining > 0){
    while(!c.available()) delay(1);
    char ch = c.read();
    remaining--;
    if(ch == '\n') break;
    if(ch != '\r') out += ch;
  }
  return true;
}

static int findPattern(const uint8_t* data, int len, const char* pat, int patLen){
  if(patLen <= 0 || len < patLen) return -1;
  for(int i=0; i <= len - patLen; i++){
    bool ok = true;
    for(int j=0; j<patLen; j++){
      if(data[i+j] != (uint8_t)pat[j]){ ok = false; break; }
    }
    if(ok) return i;
  }
  return -1;
}

static bool handleOtaMultipart(Client& c, int contentLen, const String& contentType, bool isFs, String &err){
  int b = contentType.indexOf("boundary=");
  if(b < 0){ err = "no boundary"; return false; }
  String boundary = contentType.substring(b + 9);
  boundary.trim();
  if(boundary.startsWith("\"") && boundary.endsWith("\"")){
    boundary = boundary.substring(1, boundary.length()-1);
  }
  if(boundary.length() == 0){ err = "empty boundary"; return false; }

  int remaining = contentLen;
  String line;
  readLineBody(c, remaining, line); // --boundary
  if(!line.startsWith("--" + boundary)){
    err = "bad boundary";
    return false;
  }
  // part headers
  while(true){
    if(remaining <= 0){ err = "no part header"; return false; }
    readLineBody(c, remaining, line);
    if(line.length() == 0) break; // end of headers
  }

  if(!Update.begin(UPDATE_SIZE_UNKNOWN, isFs ? U_SPIFFS : U_FLASH)){
    err = "update begin failed";
    return false;
  }

  String patStr = "\r\n--" + boundary;
  const char* pat = patStr.c_str();
  const int patLen = patStr.length();
  if(patLen <= 0){ err = "bad pattern"; Update.abort(); return false; }

  const int BUF_SIZE = 256;
  uint8_t* tail = (uint8_t*)malloc(patLen);
  uint8_t* tmp  = (uint8_t*)malloc(BUF_SIZE + patLen);
  uint8_t* buf  = (uint8_t*)malloc(BUF_SIZE);
  if(!tail || !tmp || !buf){
    if(tail) free(tail);
    if(tmp) free(tmp);
    if(buf) free(buf);
    err = "oom";
    Update.abort();
    return false;
  }
  int tailLen = 0;
  bool found = false;

  while(remaining > 0){
    int toRead = remaining > BUF_SIZE ? BUF_SIZE : remaining;
    int n = c.read(buf, toRead);
    if(n <= 0){ delay(1); continue; }
    remaining -= n;

    // build temp buffer: [tail][new]
    if(tailLen > 0) memcpy(tmp, tail, tailLen);
    memcpy(tmp + tailLen, buf, n);
    int tmpLen = tailLen + n;

    int pos = findPattern(tmp, tmpLen, pat, patLen);
    if(pos >= 0){
      if(Update.write(tmp, pos) != (size_t)pos){
        err = "write failed";
        Update.abort();
        free(tail); free(tmp);
        return false;
      }
      found = true;
      break;
    }

    if(tmpLen >= patLen){
      int writeLen = tmpLen - (patLen - 1);
      if(Update.write(tmp, writeLen) != (size_t)writeLen){
        err = "write failed";
        Update.abort();
        free(tail); free(tmp);
        return false;
      }
      memcpy(tail, tmp + writeLen, patLen - 1);
      tailLen = patLen - 1;
    } else {
      memcpy(tail, tmp, tmpLen);
      tailLen = tmpLen;
    }
  }

  free(tail);
  free(tmp);
  free(buf);

  if(!found){
    err = "boundary not found";
    Update.abort();
    return false;
  }

  while(remaining > 0){
    if(c.available()){
      c.read();
      remaining--;
    } else {
      delay(1);
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

static void rebuildRuntimeFromRules() {
  // parse shutter & reservations from current rulesDoc
  String err;
  if(!parseShutterFromRules(rulesDoc["shutters"].as<JsonArray>(), err)){
    // si règles en flash sont invalides, on désactive le volet par sécurité
    Serial.printf("[SHUTTER] invalid rules: %s -> DISABLE shutter\n", err.c_str());
    for(int s=0; s<shuttersLimit(); s++) shCfg[s].enabled = false;
    applyReservationsFromConfig();
  }
  mqttAnnounced = false;
}

// ===============================================================
// HTTP router
// ===============================================================
static void handleHttpClient(Client& client){
  String req = readLine(client); // "GET /path HTTP/1.1"
  if(req.length()==0){ client.stop(); return; }

  int contentLen = 0;
  String authHeader = "";
  String contentType = "";
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
  }

  int sp1 = req.indexOf(' ');
  int sp2 = req.indexOf(' ', sp1+1);
  String method = req.substring(0, sp1);
  String url = req.substring(sp1+1, sp2);

  String path = url;
  String query = "";
  int q = url.indexOf('?');
  if(q >= 0){ path = url.substring(0,q); query = url.substring(q+1); }
  const bool authed = checkAuthHeader(authHeader);

  // -------- routes --------
  if(method=="GET" && path=="/"){
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
      mqttCfg.enabled = (tmp["enabled"] | 0) ? true : false;
      mqttCfg.host = String((const char*)(tmp["host"] | "192.168.143.1"));
      mqttCfg.port = (uint16_t)(tmp["port"] | 1883);
      mqttCfg.user = String((const char*)(tmp["user"] | ""));
      mqttCfg.pass = String((const char*)(tmp["pass"] | ""));
      mqttCfg.clientId = String((const char*)(tmp["client_id"] | "ESPRelay4"));
      mqttCfg.base = normalizeBaseTopic(String((const char*)(tmp["base"] | "esprelay4")));
      mqttCfg.discoveryPrefix = String((const char*)(tmp["discovery_prefix"] | "homeassistant"));
      mqttCfg.retain = (tmp["retain"] | 1) ? true : false;

      if(!saveMqttCfg()){
        sendText(client, String("{\"ok\":false,\"error\":\"fs write failed\"}"), "application/json", 500);
      } else {
        mqttSetup();
        mqttAnnounced = false;
        sendText(client, String("{\"ok\":true,\"applied\":true}"), "application/json");
      }
    }
  }
  else if(method=="POST" && path=="/api/ota"){
    if(!authed){ sendAuthRequired(client); return; }
    if(contentLen <= 0 || contentType.indexOf("multipart/form-data") < 0){
      sendText(client, String("{\"ok\":false,\"error\":\"multipart required\"}"), "application/json", 400);
      client.stop();
      return;
    }
    String errMsg;
    if(!handleOtaMultipart(client, contentLen, contentType, false, errMsg)){
      sendText(client, String("{\"ok\":false,\"error\":\"") + errMsg + "\"}", "application/json", 400);
    } else {
      sendText(client, String("{\"ok\":true,\"reboot\":true}"), "application/json");
      delay(200);
      ESP.restart();
    }
  }
  else if(method=="POST" && path=="/api/otafs"){
    if(!authed){ sendAuthRequired(client); return; }
    if(contentLen <= 0 || contentType.indexOf("multipart/form-data") < 0){
      sendText(client, String("{\"ok\":false,\"error\":\"multipart required\"}"), "application/json", 400);
      client.stop();
      return;
    }
    String errMsg;
    if(!handleOtaMultipart(client, contentLen, contentType, true, errMsg)){
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
        String msg;
        JsonDocument rulesTmp;
        rulesTmp.set(tmp["rules"]);
        if(!validateAndApplyRulesDoc(rulesTmp, msg)){
          JsonDocument e;
          e["ok"]=false; e["error"]=msg;
          String out; serializeJson(e,out);
          sendText(client, out, "application/json", 400);
        } else {
          rulesDoc.clear();
          rulesDoc.set(rulesTmp);
          rebuildRuntimeFromRules();
          if(!saveRulesToFS(rulesDoc)){
            sendText(client, String("{\"ok\":false,\"error\":\"rules fs write failed\"}"), "application/json", 500);
          } else {
            String errMsg;
            if(!applyNetFromJson(tmp["net"].as<JsonObject>(), errMsg)){
              sendText(client, String("{\"ok\":false,\"error\":\"") + errMsg + "\"}", "application/json", 400);
            } else if(!applyMqttFromJson(tmp["mqtt"].as<JsonObject>(), errMsg)){
              sendText(client, String("{\"ok\":false,\"error\":\"") + errMsg + "\"}", "application/json", 400);
            } else {
              sendText(client, String("{\"ok\":true,\"applied\":true,\"reboot\":true}"), "application/json");
              delay(200);
              ESP.restart();
            }
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
      if(!validateAndApplyRulesDoc(tmp, msg)){
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
  else{
    sendText(client, "not found\n", "text/plain", 404);
  }

  delay(1);
  client.stop();
}

static void handleHttp(){
  EthernetClient ethClient = server.available();
  if(ethClient) handleHttpClient(ethClient);

  if(wifiApOn){
    WiFiClient wifiClient = wifiServer.available();
    if(wifiClient) handleHttpClient(wifiClient);
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

  // LittleFS
  if(!LittleFS.begin(true)){
    Serial.println("[FS] LittleFS init FAILED");
  } else {
    Serial.println("[FS] LittleFS OK");
    File f = LittleFS.open("/index.html", "r");
    if(f){ Serial.printf("[FS] /index.html size=%u bytes\n", (unsigned)f.size()); f.close(); }
    else Serial.println("[FS] /index.html NOT found (run uploadfs)");
  }
  if(factoryResetHeld()) doFactoryReset();
  loadAuthCfg();

  // I2C
  Serial.printf("[I2C] SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  // 1-Wire temp sensors
  tempSensors.begin();
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
  buildEthernetMac();
  applyNetCfg();
  ethernetPrintInfo();
  applyWifiCfg();

  // MQTT
  loadMqttCfg();
  mqttSetup();

  digitalWrite(PIN_LED, 1);
  Serial.println("[BOOT] Ready. Open http://<IP>/");
}

void loop() {
  handleHttp();
  updateWifiState();

  // read inputs
  pcaReadInputs();

  // tick shutter BEFORE computing simple rules, so it can use prevInputs if needed
  shutterTick();

  // compute simple rules (for all relays)
  evalSimpleRules();

  // build final outputs with ownership rules:
  // simple -> shutter overwrites reserved -> overrides (non-reserved only) -> final safety
  buildFinalRelays();

  // apply outputs
  pcaApplyRelays();

  // Temperature polling (non-blocking-ish)
  if(millis() - lastTempReadMs > 5000){
    lastTempReadMs = millis();
    if(tempCount > 0){
      tempSensors.requestTemperatures();
      for(uint8_t i=0;i<tempCount;i++){
        float c = tempSensors.getTempC(tempAddr[i]);
        tempC[i] = c;
      }
    }
    float dhtC = dht.readTemperature();
    float dhtH = dht.readHumidity();
    if(!isnan(dhtC) || !isnan(dhtH)){
      if(!dhtPresent) mqttAnnounced = false;
      if(!dhtPresent) Serial.println("[DHT] detected");
      dhtPresent = true;
      if(!isnan(dhtC)) dhtTempC = dhtC;
      if(!isnan(dhtH)) dhtHum = dhtH;
      dhtCheckDone = true;
    } else if(!dhtCheckDone) {
      Serial.println("[DHT] not detected");
      dhtCheckDone = true;
    }
  }

  // MQTT
  mqttLoop();


  // update prev inputs for edge-based rules/toggle/pulse
  for(int k=0;k<totalInputs;k++) prevInputs[k] = inputs[k];

  // 1Hz log
  static uint32_t t0 = 0;
  if(millis() - t0 > 1000){
    t0 = millis();
    String e, r, res;
    for(int i=0;i<totalInputs;i++) e += String(inputs[i] ? 1 : 0);
    for(int i=0;i<totalRelays;i++) r += String(relays[i] ? 1 : 0);
    for(int i=0;i<totalRelays;i++) res += String(reservedByShutter[i] ? 1 : 0);
    Serial.printf("[STATE] E=%s  R=%s  RES=%s\n",
      e.c_str(), r.c_str(), res.c_str()
    );
  }

  delay(10);
}
