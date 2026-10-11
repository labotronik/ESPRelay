"""Compile the actual BLE parser and loop handler with fake transport/hardware."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / 'src/main.cpp').read_text()
start = source.index('static void bleProcessCommands()')
end = source.index('static void mqttCallbackEth', start)
handler = source[start:end].replace('String(command.index)', 'std::to_string(command.index)')
harness = r'''
#include "ble_command.h"
#include <cassert>
#include <string>
using String = std::string;
using byte = unsigned char;
constexpr int pdTRUE = 1;
bool bleEnabled=true, bleClientConnected=true, reservedByShutter[16]={};
unsigned int totalRelays=4, totalInputs=4;
void* bleCommandQueue=reinterpret_cast<void*>(1);
struct Characteristic {
  std::string response;
  void setValue(const char* value) { response=value; }
} characteristic;
Characteristic* bleCommandChar=&characteristic;
BleCommand pending{};
bool queued=false;
int identifications=0;
void startIdentification() { ++identifications; }
int calls=0;
String lastTopic, lastAction;
int xQueueReceive(void*, BleCommand* out, int) {
  if (!queued) return 0;
  *out=pending; queued=false; return pdTRUE;
}
String mqttBaseTopic() { return "esprelay4/ABC"; }
void mqttHandleMessage(const char* source, char* topic, byte* payload, unsigned int length) {
  assert(std::string(source)=="BLE");
  ++calls; lastTopic=topic; lastAction.assign(reinterpret_cast<char*>(payload), length);
}
'''
harness += handler
harness += r'''
void send(const std::string& text) {
  assert(parseBleCommand(text.data(), text.size(), pending));
  queued=true; characteristic.response="PENDING"; bleProcessCommands();
}
int main() {
  BleCommand command{};
  for (const auto* text : {"R1:ON", "R4:OFF", "R16:AUTO", "V1:TOGGLE"})
    assert(parseBleCommand(text, std::strlen(text), command));
  for (const auto* text : {"R0:ON", "R17:ON", "X1:ON", "R:ON", "R1:ONjunk", "V1:AUTO", "R1:", "R1:ON\n", "R99999:ON"})
    assert(!parseBleCommand(text, std::strlen(text), command));
  const char embedded[]={'R','1',':','O','N','\0','x'};
  assert(!parseBleCommand(embedded,sizeof(embedded),command));
  send("IDENTIFY");
  assert(identifications==1 && calls==0 && characteristic.response=="OK");
  assert(!parseBleCommand("IDENTIFYx",9,command));
  send("R1:ON");
  assert(calls==1 && lastTopic=="esprelay4/ABC/relay/1/set" && lastAction=="ON");
  assert(characteristic.response=="OK");
  send("R4:AUTO"); assert(calls==2 && lastAction=="AUTO");
  send("V2:TOGGLE");
  assert(calls==3 && lastTopic=="esprelay4/ABC/vin/2/set" && lastAction=="TOGGLE");
  reservedByShutter[0]=true;
  send("R1:ON"); assert(calls==3 && characteristic.response=="ERROR:shutter relay");
  send("R5:ON"); assert(calls==3 && characteristic.response=="ERROR:invalid index");
  send("V5:ON"); assert(calls==3 && characteristic.response=="ERROR:invalid index");
  bleClientConnected=false; send("R2:OFF"); assert(calls==3);
  queued=false; bleClientConnected=true;
  bleEnabled=false; send("R2:OFF"); assert(calls==3);
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'ble_commands.cpp'
    binary = Path(directory) / 'ble_commands'
    cpp.write_text(harness)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(project / 'include'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('BLE parser, dispatch, bounds and shutter protection: OK')
