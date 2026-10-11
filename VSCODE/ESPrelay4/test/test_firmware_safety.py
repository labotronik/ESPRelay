"""Compile actual firmware functions on the host; fake hardware and failed FS writes.

Requires c++ and the project's installed ArduinoJson headers (pio run first).
Only Arduino String formatting is adapted to std::string in the host harness.
"""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / "src/main.cpp").read_text()

def section(start, end):
    return source[source.index(start):source.index(end, source.index(start))]

blocks = [
    section('static String readRawFile', '// WiFi AP helpers'),
    section('static bool pcaInitModule', '// Rules defaults + load/save'),
    section('static void addDefaultRelay', 'static bool saveRulesToFS'),
    section('static bool validDuration', 'static bool loadRulesFromFS'),
    section('enum ShutterMove', '// MQTT config (LittleFS)'),
    section('static bool inRangeInput', 'static bool getInputN'),
    section('static bool getInputN', 'static ShutterMove shutterComputeDemandFromButtons'),
    section('static bool applyDelays', '// HTTP helpers'),
    section('static bool sameShutterControl', '// HTTP router'),
    section('static void ethernetMaintainTick()', 'void loop()'),
    section('static void mqttPublishControlStates()', 'static void mqttLoop()'),
]
code = '\n'.join(blocks)
thermal = section('  // Temperature polling', '  // 1Hz log')
code += '\nvoid temperatureTick() {\n' + thermal + '\n}\n'

sim_check = section('  if (!simPinChecked) {', '  if (!modem.isNetworkConnected())')
code += '\nbool checkSim() {\n' + sim_check.replace('    ccid.trim();', '') + '\nreturn true;\n}\n'
code = code.replace('static void pcaApplyRelays() {', 'static void pcaApplyRelays() { ++outputWrites;')
for expression in ('i + 1', 'i+1', 's+1', 'totalInputs'):
    code = code.replace('String(' + expression + ')', 'std::to_string(' + expression + ')')
code = code.replace('type.toLowerCase();', 'std::transform(type.begin(), type.end(), type.begin(), ::tolower);')
code = code.replace('typeText.toLowerCase();', 'std::transform(typeText.begin(), typeText.end(), typeText.begin(), ::tolower);')
# Arduino String has overloads that std::string does not need; all numeric formatting is adapted above.
preamble = r'''
#include <ArduinoJson.h>
#include "control_time.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
using String = std::string;
using std::min; using std::max; using std::isnan;
uint32_t now = 0;
uint32_t millis() { return now; }
constexpr int MAX_INPUTS=16, MAX_RELAYS=16, PCA_MAX_MODULES=4, RELAYS_PER_MODULE=4, INPUTS_PER_MODULE=4, SHUTTER_MAX=8;
constexpr int REG_INPUT=0, REG_OUTPUT=1, REG_POL=2, REG_CFG=3, PCA_BASE_ADDR=0x70;
constexpr bool RELAY_ACTIVE_LOW=false;
constexpr uint32_t INPUT_DEBOUNCE_MS=20;
uint8_t totalInputs=4, totalRelays=4, pcaCount=1;
bool inputs[16]={}, rawInputs[16]={}, prevInputs[16]={}, inputChangePending[16]={};
uint32_t inputChangeMs[16]={};
bool combinedInputs[16]={}, prevCombinedInputs[16]={}, virtualInputs[16]={};
enum VirtualInputType : uint8_t {VIN_SWITCH=0,VIN_MOMENTARY=1};
constexpr uint32_t VINPUT_DEFAULT_PULSE_MS=250;
VirtualInputType virtualInputTypes[16]={};
uint32_t virtualInputPulseMs[16]={}, virtualInputPulseUntilMs[16]={};
bool virtualInputPulseActive[16]={};
bool toggleState[16]={}, pulseActive[16]={}, hasPending[16]={}, pendingTarget[16]={};
uint32_t pulseUntilMs[16]={}, pendingDeadlineMs[16]={};
bool reservedByShutter[16]={}, relays[16]={}, relayFromSimple[16]={}, relayFromShutter[16]={};
int8_t overrideRelay[16]={};
bool mqttAnnouncedEth=false, mqttAnnouncedGsm=false;
bool pcaPresent[4]={}, pcaAlive[4]={};
uint8_t pcaOutCache[4]={}, pcaFailCount[4]={};
int8_t lastRelayModePub[16]={};
bool pcaOutputsVerified[4]={}; uint8_t pcaWrittenOutputs[4]={}; uint32_t pcaOutputFailures[4]={};
bool lastInputsPub[16]={}, lastVirtualPub[16]={}, lastRelaysPub[16]={};
int lastShutterMove[8]={};
struct MqttConfig { bool enabled=true, retain=true; } mqttCfg;
bool publishOk=true, mqttConnected=true, mqttFastModeActive=true;
std::vector<std::pair<String,String>> publications;
bool mqttEthConnectedSafe(){return mqttConnected;} bool mqttGsmConnectedSafe(){return false;}
String mqttBaseTopic(){return "board";}
const char* relayModeText(int8_t mode){return mode==-1 ? "AUTO" : (mode==1 ? "FORCE_ON" : "FORCE_OFF");}
bool mqttPublish(const String& topic,const String& payload,bool){
  if(!publishOk) return false;
  publications.emplace_back(topic,payload); return true;
}
uint32_t pcaLastOkMs[4]={};
JsonDocument rulesDoc;
struct Logger { void println(const char*) {} template<class... T> void printf(const char*,T...) {} } Serial;
std::map<String,String> files;
bool shortWrite=false, failRename=false, failOpen=false;
struct File {
  String path; bool valid;
  operator bool() const { return valid; }
  String readString() { return files[path]; }
  size_t write(const uint8_t* data,size_t size) {
    const size_t written=shortWrite ? size-1 : size;
    files[path]=String((const char*)data,written); return written;
  }
  void flush() {} void close() {}
};
struct Filesystem {
  bool exists(const String& p) { return files.count(p); }
  File open(const String& p,const char* mode) {
    if(failOpen) return {p,false};
    if(*mode=='w') files[p].clear();
    return {p,true};
  }
  void remove(const String& p) { files.erase(p); }
  bool rename(const String& from,const String& to) {
    if(failRename) return false;
    files[to]=files[from]; files.erase(from); return true;
  }
} LittleFS;
bool hardwarePresent[4]={true,false,false,false};
uint8_t registers[4][4]={{0,255,0,255}};
std::vector<int> registerWrites;
bool failOutputWrite=false, failOutputRead=false, wrongOutputReadback=false;
struct Bus {
  int address=0;
  void beginTransmission(int addr) { address=addr; }
  int endTransmission(bool) { return hardwarePresent[address-PCA_BASE_ADDR] ? 0 : 1; }
} Wire;
bool i2cWriteReg8(uint8_t addr,uint8_t reg,uint8_t value) {
  if(!hardwarePresent[addr-PCA_BASE_ADDR]) return false;
  if(reg==REG_OUTPUT && failOutputWrite) return false;
  registerWrites.push_back(reg); registers[addr-PCA_BASE_ADDR][reg]=value; return true;
}
bool i2cReadReg8(uint8_t addr,uint8_t reg,uint8_t& value) {
  if(!hardwarePresent[addr-PCA_BASE_ADDR]) return false;
  if(reg==REG_OUTPUT && failOutputRead) return false;
  value=registers[addr-PCA_BASE_ADDR][reg];
  if(reg==REG_OUTPUT && wrongOutputReadback) value^=2;
  return true;
}
int outputWrites=0;
static void pcaApplyRelays();

constexpr int TEMP_MAX_SENSORS=2, DEVICE_DISCONNECTED_C=-127;
int tempCount=1, tempAddr[2]={};
float tempC[2]={NAN,NAN}, lastTempPub[2]={NAN,NAN};
bool tempValid[2]={}, tempSampled[2]={}, tempConversionPending=false;
uint32_t tempSampleMs[2]={}, tempConversionStartedMs=0, lastTempReadMs=0;
struct Thermometer {
  int requests=0, reads=0;
  uint32_t started=0;
  float next=25.5f;
  int getResolution() { return 12; }
  uint32_t millisToWaitForConversion(int) { return 750; }
  void requestTemperatures() { ++requests; started=now; }
  float getTempC(int) { assert((uint32_t)(now-started)>=750); ++reads; return next; }
} tempSensors;
bool dhtPresent=false, dhtCheckDone=true, dhtProbeScheduled=false;
float dhtTempC=NAN, dhtHum=NAN;
uint32_t dhtNextProbeMs=0;
struct Dht {float readTemperature(){return NAN;} float readHumidity(){return NAN;}} dht;
struct Ip { int value; String toString(){return std::to_string(value);} bool operator!=(Ip other){return value!=other.value;} };
using IPAddress=Ip;
constexpr int LinkON=1;
struct Net {bool dhcp=false;} netCfg;
struct Eth {
  int calls=0, result=0; Ip ip{1};
  int linkStatus(){return LinkON;}
  Ip localIP(){return ip;}
  int maintain(){++calls; if(result==2 || result==4) ip.value=2; return result;}
} Ethernet;
struct Mqtt {int disconnects=0; void disconnect(){++disconnects;}} mqttClientEth;
String lastIpPubEth;
uint32_t mqttLastConnectEthMs=0;
struct Server {int starts=0; void begin(){++starts;}} server;

constexpr int SIM_READY=1, SIM_LOCKED=2, SIM_ANTITHEFT_LOCKED=3;
const char SIM_PIN[]="1234";
bool simPinChecked=false, simPinUnlockBlocked=false;
String gsmLastCcid;
int gsmDownCalls=0;
void gsmMarkDown(){++gsmDownCalls;}
void gsmDebug1nce(const char*,bool){}
void delay(int ms){now+=ms;}
struct Modem {
  int status=SIM_READY, unlocks=0; bool unlockOk=true;
  int getSimStatus(){return status;}
  bool simUnlock(const char*){++unlocks; if(unlockOk) status=SIM_READY; return unlockOk;}
  String getSimCCID(){return "test";}
} modem;
'''
checks = r'''
int main() {
  // R06: output OFF is loaded while pins are still inputs.
  uint8_t cache=255;
  assert(pcaInitModule(PCA_BASE_ADDR,cache));
  assert(registerWrites.size()==3 && registerWrites[1]==REG_OUTPUT && registerWrites[2]==REG_CFG);
  assert((registers[0][REG_OUTPUT]&15)==0 && registers[0][REG_CFG]==0xF0);
  // R11: short writes and failed rename never replace the committed document.
  assert(writeFile("/net.json", "{\"mode\":\"dhcp\"}"));
  const String committed=files["/settings.json"];
  shortWrite=true; assert(!writeFile("/net.json","{\"mode\":\"static\"}"));
  assert(files["/settings.json"]==committed); shortWrite=false;
  failRename=true; assert(!writeFile("/net.json","{\"mode\":\"static\"}"));
  assert(files["/settings.json"]==committed); failRename=false;
  ConfigFileUpdate updates[]={{"/net.json","{\"mode\":\"static\"}"},{"/mqtt.json","{\"enabled\":0}"}};
  assert(commitConfigFiles(updates,2));
  assert(readFile("/net.json")=="{\"mode\":\"static\"}" && readFile("/mqtt.json")=="{\"enabled\":0}");
  const String transaction=files["/settings.json"];
  failRename=true; assert(!commitConfigFiles(updates,2)); assert(files["/settings.json"]==transaction); failRename=false;
  assert(!writeFile("/rules.json","{broken"));
  // Exact board configuration: momentary VInput2 -> R2 PULSE_RISE (500 ms), AUTO.
  totalInputs=totalRelays=4; setDefaultRules();
  rulesDoc["virtual_inputs"][1]["type"]="momentary";
  rulesDoc["virtual_inputs"][1]["pulse_ms"]=500;
  rulesDoc["relays"][1]["expr"]["op"]="PULSE_RISE";
  rulesDoc["relays"][1]["expr"]["in"]=2;
  rulesDoc["relays"][1]["pulseMs"]=500;
  String momentaryError; assert(validateRelayRules(rulesDoc,momentaryError));
  assert(normalizeVirtualInputsConfig(rulesDoc,momentaryError)); rebuildRuntimeFromRules();
  for(int i=0;i<MAX_RELAYS;i++) overrideRelay[i]=-1;
  now=100; virtualInputs[1]=true; virtualInputPulseActive[1]=true; virtualInputPulseUntilMs[1]=600;
  for(int i=0;i<totalInputs;i++) combinedInputs[i]=inputs[i] || virtualInputs[i];
  evalSimpleRules(); buildFinalRelays();
  assert(pulseActive[1] && relays[1]);
  pcaPresent[0]=true; registers[0][REG_CFG]=0xFF; pcaApplyRelays();
  assert(registers[0][REG_CFG]==0xF0); // Direction is repaired before a short pulse expires.
  assert(pcaOutputsVerified[0] && (pcaWrittenOutputs[0]&2) && (registers[0][REG_OUTPUT]&2));
  mqttFastModeActive=true; mqttPublishControlStates(); // a pulse is still published during telemetry cooldown.
  assert(lastRelaysPub[1] && lastVirtualPub[1]);
  prevCombinedInputs[1]=true;
  now=599; virtualInputTick(); evalSimpleRules(); buildFinalRelays(); assert(relays[1]);
  now=600; virtualInputTick();
  for(int i=0;i<totalInputs;i++) combinedInputs[i]=inputs[i] || virtualInputs[i];
  evalSimpleRules(); buildFinalRelays(); assert(!relays[1] && !virtualInputs[1]);
  pcaApplyRelays(); mqttPublishControlStates();
  assert(pcaOutputsVerified[0] && !(pcaWrittenOutputs[0]&2));
  std::vector<String> relayEdges, virtualEdges;
  for (const auto& message : publications) {
    if (message.first=="board/relay/2/state") relayEdges.push_back(message.second);
    if (message.first=="board/vin/2/state") virtualEdges.push_back(message.second);
  }
  assert((relayEdges==std::vector<String>{"ON","OFF"}));
  assert((virtualEdges==std::vector<String>{"ON","OFF"}));
  // Failed publications do not consume a state change; retry it on the next tick.
  relays[1]=true; publishOk=false; mqttPublishControlStates(); assert(!lastRelaysPub[1]);
  publishOk=true; mqttPublishControlStates(); assert(lastRelaysPub[1]);
  relays[1]=false; mqttPublishControlStates();
  // A missing or faulty PCA must not be reported as a confirmed output.
  relays[1]=true; failOutputWrite=true; pcaApplyRelays();
  assert(!pcaOutputsVerified[0] && pcaOutputFailures[0]==1);
  failOutputWrite=false; wrongOutputReadback=true; pcaApplyRelays();
  assert(!pcaOutputsVerified[0] && pcaOutputFailures[0]==2);
  wrongOutputReadback=false; pcaApplyRelays(); assert(pcaOutputsVerified[0]);
  relays[1]=false; failOutputRead=true; pcaApplyRelays(); assert(!pcaOutputsVerified[0]);
  failOutputRead=false; pcaApplyRelays(); assert(pcaOutputsVerified[0]);
  pcaPresent[0]=false; pcaApplyRelays(); assert(!pcaOutputsVerified[0]);
  pcaPresent[0]=true;
  prevCombinedInputs[1]=false;
  // R10: malformed operators, indices and integer durations are rejected.
  setDefaultRules(); String err; assert(validateRelayRules(rulesDoc,err));
  rulesDoc["relays"][0]["expr"]["op"]="FOLLOW";
  rulesDoc["relays"][0]["expr"]["in"]=257; assert(!validateRelayRules(rulesDoc,err));
  rulesDoc["relays"][0]["expr"]["in"]=1; assert(validateRelayRules(rulesDoc,err));
  for(auto value : { -1,600001 }) { rulesDoc["relays"][0]["onDelay"]=value; assert(!validateRelayRules(rulesDoc,err)); }
  rulesDoc["relays"][0]["onDelay"]=0.5; assert(!validateRelayRules(rulesDoc,err));
  setDefaultRules(); rulesDoc["relays"][0]["expr"]["op"]="AND";
  auto ins=rulesDoc["relays"][0]["expr"]["ins"].to<JsonArray>(); ins.add(1); ins.add(1);
  assert(!validateRelayRules(rulesDoc,err));
  JsonDocument shutters;
  deserializeJson(shutters,"[{\"up_in\":1,\"down_in\":2,\"up_relay\":257,\"down_relay\":2}]");
  assert(!parseShutterFromRules(shutters.as<JsonArray>(),err));
  // R08: saved modules remain configured when a board is absent at boot.
  totalInputs=totalRelays=16; setDefaultRules(); String saved; serializeJson(rulesDoc,saved);
  assert(writeFile("/rules.json",saved)); pcaScanAndInit(); assert(pcaCount==4 && totalRelays==16);
  hardwarePresent[1]=true; registers[1][REG_CFG]=255; now=1000; pcaReadInputs();
  assert(pcaPresent[1] && pcaAlive[1] && registers[1][REG_CFG]==0xF0 && registers[1][REG_POL]==0xF0);
  registers[0][REG_CFG]=255; registers[0][REG_POL]=0; now=2000; pcaReadInputs();
  assert(registers[0][REG_CFG]==0xF0 && registers[0][REG_POL]==0xF0);
  // R09: delay, virtual pulse, simple pulse and shutter cooldown across rollover, including deadline zero.
  now=UINT32_MAX-100; assert(!applyDelays(0,true,200,0));
  now=UINT32_MAX-1; assert(!applyDelays(0,true,200,0));
  now=98; assert(!applyDelays(0,true,200,0)); now=99; assert(applyDelays(0,true,200,0));
  virtualInputTypes[0]=VIN_MOMENTARY; virtualInputs[0]=true; virtualInputPulseActive[0]=true; virtualInputPulseUntilMs[0]=0;
  now=UINT32_MAX; virtualInputTick(); assert(virtualInputs[0]); now=0; virtualInputTick(); assert(!virtualInputs[0] && !virtualInputPulseActive[0]);
  JsonDocument expr; deserializeJson(expr,"{\"op\":\"PULSE_RISE\",\"in\":1}");
  combinedInputs[0]=true; prevCombinedInputs[0]=false; now=UINT32_MAX-9;
  assert(evalExprSimple(0,expr.as<JsonObject>(),10)); prevCombinedInputs[0]=true;
  now=UINT32_MAX; assert(evalExprSimple(0,expr.as<JsonObject>(),10)); now=0; assert(!evalExprSimple(0,expr.as<JsonObject>(),10));
  now=UINT32_MAX-9; assert(!evalExprSimple(0,expr.as<JsonObject>(),10)); // An expired pulse never replays.
  shCfg[0].enabled=true; shCfg[0].up_relay=1; shCfg[0].down_relay=2;
  shRt[0].cooldownActive=true; shRt[0].cooldownUntilMs=0;
  now=UINT32_MAX; shutterCommand(0,SH_UP); assert(shRt[0].move==SH_STOP);
  now=0; shutterCommand(0,SH_STOP); assert(!shRt[0].cooldownActive);
  shutterCommand(0,SH_UP); assert(shRt[0].move==SH_UP);
  // R29: changing an unrelated rule preserves moving shutters and unchanged toggle memory.
  totalInputs=totalRelays=4; setDefaultRules();
  deserializeJson(shutters,"[{\"up_in\":1,\"down_in\":2,\"up_relay\":1,\"down_relay\":2}]");
  rulesDoc["shutters"].set(shutters.as<JsonArray>()); rebuildRuntimeFromRules();
  shRt[0].move=SH_UP; shRt[0].moveStartMs=123; shRt[0].manual=MC_UP;
  relays[0]=true; toggleState[3]=true;
  JsonDocument previous; previous.set(rulesDoc);
  rulesDoc["relays"][2]["expr"]["op"]="FOLLOW"; rulesDoc["relays"][2]["expr"]["in"]=3;
  toggleState[2]=true; pulseActive[2]=true; hasPending[2]=true;
  rebuildRuntimeFromRules(&previous);
  assert(shRt[0].move==SH_UP && shRt[0].moveStartMs==123 && shRt[0].manual==MC_UP);
  assert(toggleState[3] && !toggleState[2] && !pulseActive[2] && !hasPending[2]);
  // Changing wiring stops the old pair immediately and initializes a cooldown on the new pair.
  previous.set(rulesDoc); rulesDoc["shutters"][0]["up_relay"]=3; rulesDoc["shutters"][0]["down_relay"]=4;
  outputWrites=0; rebuildRuntimeFromRules(&previous);
  assert(!relays[0] && !relays[1] && outputWrites==1 && shRt[0].move==SH_STOP && shRt[0].cooldownActive);
  // R28: first read waits for conversion, errors become invalid, and the wait survives rollover.
  now=5000; temperatureTick(); assert(tempSensors.requests==1 && tempSensors.reads==0 && !tempValid[0]);
  now=5749; temperatureTick(); assert(tempSensors.reads==0);
  now=5750; temperatureTick(); assert(tempSensors.reads==1 && tempValid[0] && tempC[0]==25.5f && tempSampled[0]);
  now=10000; temperatureTick(); tempSensors.next=DEVICE_DISCONNECTED_C;
  now=10750; temperatureTick(); assert(!tempValid[0] && std::isnan(tempC[0]));
  now=UINT32_MAX-100; lastTempReadMs=now-5000; temperatureTick(); const int reads=tempSensors.reads;
  tempSensors.next=20.0f; now=648; temperatureTick(); assert(tempSensors.reads==reads);
  now=649; temperatureTick(); assert(tempSensors.reads==reads+1 && tempValid[0]);
  // R17: static mode skips DHCP; a renewed lease with a new IP closes the old MQTT socket.
  now=2000; ethernetMaintainTick(); assert(Ethernet.calls==0);
  netCfg.dhcp=true; Ethernet.result=2; ethernetMaintainTick();
  assert(Ethernet.calls==1 && mqttClientEth.disconnects==1 && server.starts==1);
  ethernetMaintainTick(); assert(Ethernet.calls==1);

  // R16: ready=1 succeeds, PIN locked=2 unlocks, PUK/other state=3 stays unavailable.
  assert(checkSim() && modem.unlocks==0);
  simPinChecked=false; modem.status=SIM_LOCKED; assert(checkSim() && modem.unlocks==1);
  simPinChecked=false; modem.status=SIM_ANTITHEFT_LOCKED; assert(!checkSim() && !simPinChecked && modem.unlocks==1);
  modem.status=SIM_LOCKED; modem.unlockOk=false; assert(!checkSim() && simPinUnlockBlocked && modem.unlocks==2);
  assert(!checkSim() && modem.unlocks==2); // A wrong PIN is never retried automatically.

}
'''
sim_library = (project / '.pio/libdeps/esp32s3_custom_n4_ethernet/TinyGSM/src/TinyGsmGPRS.tpp').read_text()
sim_enum = sim_library[sim_library.index('enum SimStatus'):]
sim_enum = sim_enum[:sim_enum.index('};') + 2]
preamble = preamble.replace('constexpr int SIM_READY=1, SIM_LOCKED=2, SIM_ANTITHEFT_LOCKED=3;', sim_enum)

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory)/'firmware_safety.cpp'
    binary = Path(directory)/'firmware_safety'
    cpp.write_text(preamble + code + checks)
    subprocess.run(['c++','-std=c++17','-I'+str(project/'include'),
                    '-I'+str(project/'.pio/libdeps/esp32s3_custom_n4_ethernet/ArduinoJson/src'),
                    str(cpp),'-o',str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Firmware safety checks passed (PCA recovery, atomic writes, schema, rollover, selective runtime, temperature conversion, DHCP, SIM).')
