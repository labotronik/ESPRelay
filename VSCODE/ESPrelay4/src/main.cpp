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
//   PUT  /api/rules        -> remplace les règles (validation volet + conflits)
//   PUT  /api/net          -> applique + sauvegarde config réseau
//   POST /api/override     -> override d'un relais (REFUSE si relais réservé volet)
//   POST /api/shutter      -> commande volet (UP/DOWN/STOP) (seul moyen "API" de bouger les relais volet)
//
// IMPORTANT: adapte ces pins à ton PCB : I2C_SDA/I2C_SCL et PIN_W5500_CS, PIN_LED.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Ethernet.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

// ===================== A ADAPTER A TON PCB =====================
static const uint8_t PIN_LED = 40;

// I2C (PCA9538)
static const int I2C_SDA = 8;     // <-- adapte
static const int I2C_SCL = 9;     // <-- adapte
static const uint8_t PCA_ADDR = 0x70;

// W5500 (SPI)
static const int PIN_W5500_CS = 10;  // <-- adapte
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

// ===================== Réseau (à adapter) ======================
byte mac[] = { 0x02,0x12,0x34,0x56,0x78,0x9A };

struct NetConfig {
  bool dhcp;
  IPAddress ip;
  IPAddress gw;
  IPAddress sn;
  IPAddress dns;
};

static NetConfig netCfg = {
  false,
  IPAddress(192,168,1,50),
  IPAddress(192,168,1,1),
  IPAddress(255,255,255,0),
  IPAddress(192,168,1,1)
};

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
static bool lastInputsPub[4] = {false,false,false,false};
static bool lastRelaysPub[4] = {false,false,false,false};
static int lastShutterMove = -1;


// ===================== Etat IO =====================
bool inputs[4] = {0,0,0,0};   // E1..E4 = IO4..IO7
bool prevInputs[4] = {0,0,0,0};

bool relays[4] = {0,0,0,0};   // R1..R4 = IO0..IO3 (final)
bool relayFromSimple[4] = {0,0,0,0};
bool relayFromShutter[4] = {0,0,0,0};

uint8_t pcaOutCache = 0x00;

// Overrides : -1 auto, 0 force off, 1 force on (uniquement pour relais NON réservés)
int8_t overrideRelay[4] = {-1,-1,-1,-1};

// Mémoire toggle + pulse pour règles simples
bool toggleState[4] = {0,0,0,0};
uint32_t pulseUntilMs[4] = {0,0,0,0};

// Delay state pour règles simples
bool pendingTarget[4] = {0,0,0,0};
bool hasPending[4] = {0,0,0,0};
uint32_t pendingDeadlineMs[4] = {0,0,0,0};

// Réservation des relais par volet
bool reservedByShutter[4] = {false,false,false,false};

// ===================== Règles JSON en RAM ======================
DynamicJsonDocument rulesDoc(3072);

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
  DynamicJsonDocument doc(256);
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
  DynamicJsonDocument doc(256);
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
static void sendFile(EthernetClient& client, const char* path, const char* contentType) {
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
static bool pcaInit() {
  Wire.beginTransmission(PCA_ADDR);
  if (Wire.endTransmission(true) != 0) return false;

  if (!i2cWriteReg8(PCA_ADDR, REG_POL, 0x00)) return false;
  if (!i2cWriteReg8(PCA_ADDR, REG_CFG, 0xF0)) return false; // IO0..3 out, IO4..7 in

  // outputs off
  uint8_t outNibble = RELAY_ACTIVE_LOW ? 0x0F : 0x00;
  pcaOutCache = (pcaOutCache & 0xF0) | (outNibble & 0x0F);
  if (!i2cWriteReg8(PCA_ADDR, REG_OUTPUT, pcaOutCache)) return false;

  return true;
}

static void pcaReadInputs() {
  uint8_t in = 0;
  if(!i2cReadReg8(PCA_ADDR, REG_INPUT, in)) return;
  for(int i=0;i<4;i++){
    inputs[i] = (in >> (4+i)) & 0x1; // IO4..IO7
  }
}

static void pcaApplyRelays() {
  uint8_t nibble = 0;
  for(int i=0;i<4;i++){
    bool v = relays[i];
    if(RELAY_ACTIVE_LOW) v = !v;
    if(v) nibble |= (1u << i);
  }
  pcaOutCache = (pcaOutCache & 0xF0) | (nibble & 0x0F);
  i2cWriteReg8(PCA_ADDR, REG_OUTPUT, pcaOutCache);
}

// ===============================================================
// Rules defaults + load/save
// ===============================================================
static void setDefaultRules() {
  rulesDoc.clear();
  rulesDoc["version"] = 2;

  JsonArray rel = rulesDoc.createNestedArray("relays");
  for(int i=0;i<4;i++){
    JsonObject r = rel.createNestedObject();
    JsonObject expr = r.createNestedObject("expr");
    expr["op"] = "FOLLOW";
    expr["in"] = i+1;
    r["invert"] = false;
    r["onDelay"] = 0;
    r["offDelay"] = 0;
    r["pulseMs"] = 200;
  }

  rulesDoc.createNestedArray("shutters"); // vide par défaut
}

static bool saveRulesToFS(const DynamicJsonDocument& doc) {
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
  if(!rulesDoc["relays"].is<JsonArray>() || rulesDoc["relays"].as<JsonArray>().size() != 4){
    Serial.println("[RULES] invalid relays[] -> default");
    setDefaultRules();
    saveRulesToFS(rulesDoc);
    return false;
  }
  if(!rulesDoc["shutters"].is<JsonArray>()){
    rulesDoc["shutters"] = JsonArray(); // will be fixed below by set?
    // safer:
    rulesDoc.remove("shutters");
    rulesDoc.createNestedArray("shutters");
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

ShutterCfg shCfg;
ShutterRuntime shRt;

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
  DynamicJsonDocument doc(384);
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
  DynamicJsonDocument doc(384);
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

static void mqttPublishDiscovery() {
  if (!mqttClient.connected()) return;
  String base = mqttBaseTopic();
  String node = mqttNodeId();
  String avail = base + "/status";
  String id = node;

  for (int i = 0; i < 4; i++) {
    DynamicJsonDocument doc(512);
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
    JsonObject dev = doc.createNestedObject("dev");
    dev["ids"] = id;
    dev["name"] = node;
    dev["mdl"] = "ESPRelay4";
    dev["mf"] = "ESPRelay4";
    String topic = mqttCfg.discoveryPrefix + "/switch/" + uid + "/config";
    String out; serializeJson(doc, out);
    mqttPublish(topic, out, true);
  }

  for (int i = 0; i < 4; i++) {
    DynamicJsonDocument doc(512);
    String uid = id + "_input_" + String(i+1);
    doc["name"] = "Input " + String(i+1);
    doc["uniq_id"] = uid;
    doc["stat_t"] = base + "/input/" + String(i+1) + "/state";
    doc["pl_on"] = "ON";
    doc["pl_off"] = "OFF";
    doc["avty_t"] = avail;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    JsonObject dev = doc.createNestedObject("dev");
    dev["ids"] = id;
    dev["name"] = node;
    dev["mdl"] = "ESPRelay4";
    dev["mf"] = "ESPRelay4";
    String topic = mqttCfg.discoveryPrefix + "/binary_sensor/" + uid + "/config";
    String out; serializeJson(doc, out);
    mqttPublish(topic, out, true);
  }

  if (shCfg.enabled) {
    DynamicJsonDocument doc(512);
    String uid = id + "_shutter";
    doc["name"] = shCfg.name;
    doc["uniq_id"] = uid;
    doc["cmd_t"] = base + "/shutter/set";
    doc["stat_t"] = base + "/shutter/state";
    doc["pl_open"] = "OPEN";
    doc["pl_close"] = "CLOSE";
    doc["pl_stop"] = "STOP";
    doc["avty_t"] = avail;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    JsonObject dev = doc.createNestedObject("dev");
    dev["ids"] = id;
    dev["name"] = node;
    dev["mdl"] = "ESPRelay4";
    dev["mf"] = "ESPRelay4";
    String topic = mqttCfg.discoveryPrefix + "/cover/" + uid + "/config";
    String out; serializeJson(doc, out);
    mqttPublish(topic, out, true);
  }

  mqttAnnounced = true;
}

static void mqttPublishAllState() {
  if (!mqttClient.connected()) return;
  String base = mqttBaseTopic();
  mqttPublish(base + "/status", "online", true);
  for (int i = 0; i < 4; i++) {
    mqttPublish(base + "/relay/" + String(i+1) + "/state", relays[i] ? "ON" : "OFF", mqttCfg.retain);
    mqttPublish(base + "/input/" + String(i+1) + "/state", inputs[i] ? "ON" : "OFF", mqttCfg.retain);
    lastRelaysPub[i] = relays[i];
    lastInputsPub[i] = inputs[i];
  }
  if (shCfg.enabled) {
    const char* st = (shRt.move==SH_UP ? "opening" : (shRt.move==SH_DOWN ? "closing" : "stopped"));
    mqttPublish(base + "/shutter/state", String(st), mqttCfg.retain);
    lastShutterMove = (int)shRt.move;
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
    if (idx >= 1 && idx <= 4) {
      int i = idx - 1;
      if (!reservedByShutter[i]) {
        if (p == "ON") overrideRelay[i] = 1;
        else if (p == "OFF") overrideRelay[i] = 0;
        else if (p == "AUTO") overrideRelay[i] = -1;
        else if (p == "TOGGLE") overrideRelay[i] = (overrideRelay[i] == 1 ? 0 : 1);
      }
    }
  } else if (t == base + "/shutter/set") {
    if (p == "OPEN" || p == "UP") shRt.manual = MC_UP;
    else if (p == "CLOSE" || p == "DOWN") shRt.manual = MC_DOWN;
    else if (p == "STOP") shRt.manual = MC_STOP;
  }
}

static void mqttSetup() {
  mqttCfg.base = normalizeBaseTopic(mqttCfg.base);
  mqttClient.setServer(mqttCfg.host.c_str(), mqttCfg.port);
  mqttClient.setCallback(mqttCallback);
}

static void mqttSubscribeTopics() {
  String base = mqttBaseTopic();
  for (int i = 1; i <= 4; i++) {
    mqttClient.subscribe((base + "/relay/" + String(i) + "/set").c_str());
  }
  mqttClient.subscribe((base + "/shutter/set").c_str());
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
  for (int i = 0; i < 4; i++) {
    if (inputs[i] != lastInputsPub[i]) {
      mqttPublish(base + "/input/" + String(i+1) + "/state", inputs[i] ? "ON" : "OFF", mqttCfg.retain);
      lastInputsPub[i] = inputs[i];
    }
    if (relays[i] != lastRelaysPub[i]) {
      mqttPublish(base + "/relay/" + String(i+1) + "/state", relays[i] ? "ON" : "OFF", mqttCfg.retain);
      lastRelaysPub[i] = relays[i];
    }
  }

  if (shCfg.enabled && (int)shRt.move != lastShutterMove) {
    const char* st = (shRt.move==SH_UP ? "opening" : (shRt.move==SH_DOWN ? "closing" : "stopped"));
    mqttPublish(base + "/shutter/state", String(st), mqttCfg.retain);
    lastShutterMove = (int)shRt.move;
  }
}

static bool inRange14(int v){ return v>=1 && v<=4; }

static void clearReservations() {
  for(int i=0;i<4;i++) reservedByShutter[i] = false;
}

static void applyReservationsFromConfig() {
  clearReservations();
  if(!shCfg.enabled) return;
  if(inRange14(shCfg.up_relay)) reservedByShutter[shCfg.up_relay-1] = true;
  if(inRange14(shCfg.down_relay)) reservedByShutter[shCfg.down_relay-1] = true;
}

static bool parseShutterFromRules(JsonArray shutters, String &errMsg) {
  shCfg = ShutterCfg();
  shRt = ShutterRuntime();

  if(!shutters || shutters.size()==0){
    shCfg.enabled = false;
    applyReservationsFromConfig();
    return true;
  }

  // On gère 1 volet (shutters[0]) pour l’instant
  JsonObject s0 = shutters[0].as<JsonObject>();
  if(!s0){
    shCfg.enabled = false;
    applyReservationsFromConfig();
    return true;
  }

  shCfg.enabled = true;
  shCfg.name = (const char*)(s0["name"] | "Volet");

  shCfg.up_in = (uint8_t)(int)(s0["up_in"] | 1);
  shCfg.down_in = (uint8_t)(int)(s0["down_in"] | 2);
  shCfg.up_relay = (uint8_t)(int)(s0["up_relay"] | 1);
  shCfg.down_relay = (uint8_t)(int)(s0["down_relay"] | 2);

  shCfg.mode = (const char*)(s0["mode"] | "hold");
  shCfg.priority = (const char*)(s0["priority"] | "stop");

  shCfg.deadtime_ms = (uint32_t)(uint64_t)(s0["deadtime_ms"] | 400);
  shCfg.max_run_ms  = (uint32_t)(uint64_t)(s0["max_run_ms"] | 25000);

  // validation stricte
  if(!inRange14(shCfg.up_in) || !inRange14(shCfg.down_in)){
    errMsg = "shutter: up_in/down_in must be 1..4";
    return false;
  }
  if(!inRange14(shCfg.up_relay) || !inRange14(shCfg.down_relay)){
    errMsg = "shutter: up_relay/down_relay must be 1..4";
    return false;
  }
  if(shCfg.up_relay == shCfg.down_relay){
    errMsg = "shutter: up_relay and down_relay must be different";
    return false;
  }
  if(!(shCfg.mode=="hold" || shCfg.mode=="toggle")){
    errMsg = "shutter: mode must be hold|toggle";
    return false;
  }
  if(!(shCfg.priority=="stop" || shCfg.priority=="up" || shCfg.priority=="down")){
    errMsg = "shutter: priority must be stop|up|down";
    return false;
  }
  if(shCfg.deadtime_ms > 60000) shCfg.deadtime_ms = 60000;
  if(shCfg.max_run_ms > 600000) shCfg.max_run_ms = 600000; // 10min max sécurité

  applyReservationsFromConfig();
  return true;
}

static bool getInputN(int n){ // n = 1..4
  if(n < 1 || n > 4) return false;
  return inputs[n-1];
}

static void shutterSetOutputs(ShutterMove m) {
  // sécurité absolue: jamais les deux
  bool up = (m == SH_UP);
  bool dn = (m == SH_DOWN);

  // si interlock violé (ne devrait jamais) => STOP
  if(up && dn){
    up = false; dn = false; m = SH_STOP;
  }

  // écrire dans relayFromShutter sur les relais réservés
  for(int i=0;i<4;i++) relayFromShutter[i] = false;
  if(!shCfg.enabled) return;

  relayFromShutter[shCfg.up_relay-1] = up;
  relayFromShutter[shCfg.down_relay-1] = dn;
}

static void shutterForceStop() {
  shRt.move = SH_STOP;
  shRt.manual = MC_NONE; // option: STOP efface le manuel
  shutterSetOutputs(SH_STOP);
}

static void shutterCommand(ShutterMove req) {
  // gestion dead-time entre inversions
  uint32_t now = millis();

  if(req == SH_STOP){
    shRt.move = SH_STOP;
    shutterSetOutputs(SH_STOP);
    return;
  }

  // si cooldown actif, on reste STOP jusqu’à expiration
  if(now < shRt.cooldownUntilMs){
    shRt.move = SH_STOP;
    shutterSetOutputs(SH_STOP);
    return;
  }

  // si changement de sens alors qu’on bouge -> passer STOP + cooldown
  if(shRt.move != SH_STOP && shRt.move != req){
    shRt.move = SH_STOP;
    shutterSetOutputs(SH_STOP);
    shRt.cooldownUntilMs = now + shCfg.deadtime_ms;
    return; // la prochaine itération autorisera req après cooldown
  }

  // sinon, démarrer/continuer
  if(shRt.move != req){
    shRt.move = req;
    shRt.moveStartMs = now;
  }

  shutterSetOutputs(req);
}

static ShutterMove shutterComputeDemandFromButtons() {
  bool upBtn = getInputN(shCfg.up_in);
  bool dnBtn = getInputN(shCfg.down_in);

  // priorité si les deux
  if(upBtn && dnBtn){
    if(shCfg.priority=="up") return SH_UP;
    if(shCfg.priority=="down") return SH_DOWN;
    return SH_STOP; // stop
  }
  if(upBtn) return SH_UP;
  if(dnBtn) return SH_DOWN;
  return SH_STOP;
}

static void shutterTick() {
  // clear outputs if disabled
  if(!shCfg.enabled){
    for(int i=0;i<4;i++) relayFromShutter[i] = false;
    return;
  }

  uint32_t now = millis();

  // sécurité max-run
  if(shCfg.max_run_ms > 0 && shRt.move != SH_STOP){
    if(now - shRt.moveStartMs >= shCfg.max_run_ms){
      shutterForceStop();
      return;
    }
  }

  // demande depuis API manual (si active)
  // MC_UP / MC_DOWN persistent jusqu’à MC_STOP ou max_run
  ShutterMove demand = SH_STOP;

  if(shRt.manual == MC_STOP){
    shutterForceStop();
    return;
  }
  if(shRt.manual == MC_UP) demand = SH_UP;
  else if(shRt.manual == MC_DOWN) demand = SH_DOWN;
  else {
    // mode selon cfg
    if(shCfg.mode == "hold"){
      demand = shutterComputeDemandFromButtons();
    } else {
      // toggle : front montant sur up ou down
      bool upBtn = getInputN(shCfg.up_in);
      bool dnBtn = getInputN(shCfg.down_in);

      bool upRise = upBtn && !shRt.lastUpBtn;
      bool dnRise = dnBtn && !shRt.lastDownBtn;

      shRt.lastUpBtn = upBtn;
      shRt.lastDownBtn = dnBtn;

      // si les deux rises simultanés -> STOP (sécurité)
      if(upRise && dnRise){
        demand = SH_STOP;
        shutterCommand(SH_STOP);
        return;
      }

      if(upRise){
        if(shRt.move == SH_UP) demand = SH_STOP;
        else demand = SH_UP;
      } else if(dnRise){
        if(shRt.move == SH_DOWN) demand = SH_STOP;
        else demand = SH_DOWN;
      } else {
        // pas de nouvel évènement : garder l’état courant
        demand = shRt.move;
      }
    }
  }

  // en mode hold, si aucun bouton et pas manuel -> STOP
  if(shCfg.mode=="hold" && shRt.manual==MC_NONE){
    shutterCommand(demand);
    return;
  }

  // toggle / manuel : appliquer demand
  shutterCommand(demand);
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
    if(in>=1 && in<=4){
      thisRise = (inputs[in-1] && !prevInputs[in-1]);
    }
    if(thisRise) toggleState[relayIndex] = !toggleState[relayIndex];
    return toggleState[relayIndex];
  }
  if(strcmp(op,"PULSE_RISE")==0){
    int in = expr["in"] | 1;
    uint32_t pulseMs = expr["pulseMs"] | 200;
    bool thisRise = false;
    if(in>=1 && in<=4){
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
  for(int i=0;i<4;i++){
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
  for(int i=0;i<4;i++){
    relays[i] = relayFromSimple[i];
  }

  // 2) apply shutter ownership: for each reserved relay, shutter output wins
  if(shCfg.enabled){
    // garantie interlock déjà dans shutterSetOutputs()
    relays[shCfg.up_relay-1]   = relayFromShutter[shCfg.up_relay-1];
    relays[shCfg.down_relay-1] = relayFromShutter[shCfg.down_relay-1];
  }

  // 3) apply override ONLY for non-reserved relays
  for(int i=0;i<4;i++){
    if(reservedByShutter[i]) continue; // PROTECTION: cannot override shutter relays
    if(overrideRelay[i] == -1) continue;
    relays[i] = (overrideRelay[i] == 1);
  }

  // 4) final safety (absolute): if shutter relays both ON => STOP both
  if(shCfg.enabled){
    bool up = relays[shCfg.up_relay-1];
    bool dn = relays[shCfg.down_relay-1];
    if(up && dn){
      relays[shCfg.up_relay-1] = false;
      relays[shCfg.down_relay-1] = false;
    }
  }
}

// ===============================================================
// HTTP helpers
// ===============================================================
static String readLine(EthernetClient& c){
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

static String readBody(EthernetClient& c, int len){
  String b; b.reserve(len);
  while(len-- > 0){
    while(!c.available()) delay(1);
    b += (char)c.read();
  }
  return b;
}

static void sendText(EthernetClient& c, const String& body, const char* ctype, int code=200){
  if(code==200) c.println("HTTP/1.1 200 OK");
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

static void sendJsonState(EthernetClient& c){
  DynamicJsonDocument doc(768);
  JsonArray inA = doc.createNestedArray("inputs");
  JsonArray reA = doc.createNestedArray("relays");
  JsonArray ovA = doc.createNestedArray("override");
  JsonArray rsA = doc.createNestedArray("reserved");

  for(int i=0;i<4;i++){
    inA.add(inputs[i] ? 1 : 0);
    reA.add(relays[i] ? 1 : 0);
    ovA.add(overrideRelay[i]);
    rsA.add(reservedByShutter[i] ? 1 : 0);
  }

  JsonObject eth = doc.createNestedObject("eth");
  eth["link"] = (Ethernet.linkStatus()==LinkON) ? 1 : 0;
  eth["ip"] = Ethernet.localIP().toString();

  JsonObject sh = doc.createNestedObject("shutter");
  sh["enabled"] = shCfg.enabled ? 1 : 0;
  if(shCfg.enabled){
    sh["name"] = shCfg.name;
    sh["up_relay"] = shCfg.up_relay;
    sh["down_relay"] = shCfg.down_relay;
    sh["move"] = (shRt.move==SH_UP ? "up" : (shRt.move==SH_DOWN ? "down" : "stop"));
    sh["cooldown_ms"] = (millis() < shRt.cooldownUntilMs) ? (uint32_t)(shRt.cooldownUntilMs - millis()) : 0;
  }

  doc["uptime_ms"] = (uint32_t)millis();

  String out; serializeJson(doc, out);
  sendText(c, out, "application/json");
}

static void sendJsonNetCfg(EthernetClient& c){
  // Reload from flash to reflect latest saved mode
  loadNetCfg();
  String out;
  netCfgToJson(out);
  sendText(c, out, "application/json");
}

static void sendJsonMqttCfg(EthernetClient& c){
  loadMqttCfg();
  mqttSetup();
  String out;
  mqttCfgToJson(out);
  sendText(c, out, "application/json");
}

static void sendJsonRules(EthernetClient& c){
  String out;
  serializeJsonPretty(rulesDoc, out);
  sendText(c, out, "application/json");
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
static bool validateAndApplyRulesDoc(DynamicJsonDocument &candidate, String &errMsg) {
  // relays must be array size 4
  if(!candidate["relays"].is<JsonArray>() || candidate["relays"].as<JsonArray>().size()!=4){
    errMsg = "relays must be array size 4";
    return false;
  }
  // shutters optional, but if present must be array
  if(candidate.containsKey("shutters") && !candidate["shutters"].is<JsonArray>()){
    errMsg = "shutters must be array";
    return false;
  }
  if(!candidate.containsKey("shutters")){
    candidate.createNestedArray("shutters");
  }
  if(!candidate.containsKey("version")) candidate["version"] = 2;

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
    shCfg.enabled = false;
    applyReservationsFromConfig();
  }
  mqttAnnounced = false;
}

// ===============================================================
// HTTP router
// ===============================================================
static void handleHttp(){
  EthernetClient client = server.available();
  if(!client) return;

  String req = readLine(client); // "GET /path HTTP/1.1"
  if(req.length()==0){ client.stop(); return; }

  int contentLen = 0;
  while(true){
    String h = readLine(client);
    if(h.length()==0) break;
    if(h.startsWith("Content-Length:")){
      contentLen = h.substring(15).toInt();
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

  // -------- routes --------
  if(method=="GET" && path=="/"){
    sendFile(client, "/index.html", "text/html; charset=utf-8");
  }
  else if(method=="GET" && path=="/api/state"){
    sendJsonState(client);
  }
  else if(method=="GET" && path=="/api/rules"){
    sendJsonRules(client);
  }
  else if(method=="GET" && path=="/api/net"){
    sendJsonNetCfg(client);
  }
  else if(method=="GET" && path=="/api/mqtt"){
    sendJsonMqttCfg(client);
  }
  else if(method=="PUT" && path=="/api/net"){
    String body = readBody(client, contentLen);
    DynamicJsonDocument tmp(256);
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
  else if(method=="PUT" && path=="/api/mqtt"){
    String body = readBody(client, contentLen);
    DynamicJsonDocument tmp(512);
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
  else if(method=="PUT" && path=="/api/rules"){
    String body = readBody(client, contentLen);

    DynamicJsonDocument tmp(3072);
    auto err = deserializeJson(tmp, body);
    if(err){
      sendText(client, String("{\"ok\":false,\"error\":\"bad json\"}"), "application/json", 400);
    } else {
      String msg;
      if(!validateAndApplyRulesDoc(tmp, msg)){
        DynamicJsonDocument e(256);
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
    // Strict protection: refuse override on reserved relays
    String body = readBody(client, contentLen);
    DynamicJsonDocument doc(256);
    auto err = deserializeJson(doc, body);
    if(err){
      sendText(client, String("{\"ok\":false,\"error\":\"bad json\"}"), "application/json", 400);
    } else {
      int r = doc["relay"] | 1; // 1..4
      const char* mode = doc["mode"] | "AUTO";
      if(r < 1 || r > 4){
        sendText(client, String("{\"ok\":false,\"error\":\"relay must be 1..4\"}"), "application/json", 400);
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
    // Commande volet: { "cmd":"UP|DOWN|STOP" }
    String body = readBody(client, contentLen);
    DynamicJsonDocument doc(256);
    auto err = deserializeJson(doc, body);
    if(err){
      sendText(client, String("{\"ok\":false,\"error\":\"bad json\"}"), "application/json", 400);
    } else {
      const char* cmd = doc["cmd"] | "STOP";
      if(!shCfg.enabled){
        sendText(client, String("{\"ok\":false,\"error\":\"no shutter configured\"}"), "application/json", 400);
      } else if(strcmp(cmd,"UP")==0){
        shRt.manual = MC_UP;
        sendText(client, String("{\"ok\":true}"), "application/json");
      } else if(strcmp(cmd,"DOWN")==0){
        shRt.manual = MC_DOWN;
        sendText(client, String("{\"ok\":true}"), "application/json");
      } else if(strcmp(cmd,"STOP")==0){
        shRt.manual = MC_STOP;
        sendText(client, String("{\"ok\":true}"), "application/json");
      } else if(strcmp(cmd,"AUTO")==0){
        // option: rendre la main aux boutons (désactive le manuel)
        shRt.manual = MC_NONE;
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

  // I2C
  Serial.printf("[I2C] SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  // PCA9538
  Serial.printf("[PCA9538] addr=0x%02X init...\n", PCA_ADDR);
  if(!pcaInit()){
    Serial.println("[PCA9538] INIT FAILED (wiring/pullups/address)");
  } else {
    Serial.println("[PCA9538] INIT OK");
  }

  // Rules
  loadRulesFromFS();
  rebuildRuntimeFromRules();

  // Ethernet
  loadNetCfg();
  applyNetCfg();
  ethernetPrintInfo();

  // MQTT
  loadMqttCfg();
  mqttSetup();

  digitalWrite(PIN_LED, 1);
  Serial.println("[BOOT] Ready. Open http://<IP>/");
}

void loop() {
  handleHttp();

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

  // MQTT
  mqttLoop();

  // update prev inputs for edge-based rules/toggle/pulse
  for(int k=0;k<4;k++) prevInputs[k] = inputs[k];

  // 1Hz log
  static uint32_t t0 = 0;
  if(millis() - t0 > 1000){
    t0 = millis();
    Serial.printf("[STATE] E=%d%d%d%d  R=%d%d%d%d  RES=%d%d%d%d  SH=%s\n",
      (int)inputs[0],(int)inputs[1],(int)inputs[2],(int)inputs[3],
      (int)relays[0],(int)relays[1],(int)relays[2],(int)relays[3],
      (int)reservedByShutter[0],(int)reservedByShutter[1],(int)reservedByShutter[2],(int)reservedByShutter[3],
      shCfg.enabled ? (shRt.move==SH_UP?"UP":(shRt.move==SH_DOWN?"DOWN":"STOP")) : "DISABLED"
    );
  }

  delay(10);
}
