"""Host regression test of the firmware's actual input/actuation loop.

Run with: python3 test/test_input_loop.py (requires a C++ compiler).
Hardware and background tasks are stubbed; scheduling uses the source loop.
"""
from pathlib import Path
import subprocess
import tempfile


project = Path(__file__).resolve().parents[1]
source = (project / "src/main.cpp").read_text()
loop = source.split("void loop() {", 1)[1].split("  // Temperature polling", 1)[0]
stubs = r"""
#include <cassert>
#include <cstdint>
const int totalInputs = 1;
bool inputs[1]={}, prevInputs[1]={}, virtualInputs[1]={};
bool combinedInputs[1]={}, prevCombinedInputs[1]={};
bool mqttFastCommandPending=false;
uint32_t now=100, deadline=0;
bool command=false, bleCommand=false, blockMqtt=false, output=false, toggle=false;
int evaluations=0, rises=0;
void handleHttp() {} void pcaReadInputs() {} void debounceInputs() {}
void virtualInputTick() {
  if(deadline && now>=deadline) {virtualInputs[0]=false; deadline=0;}
}
void mqttLoop() {
  if(command) {
    virtualInputs[0]=true; deadline=now+10;
    mqttFastCommandPending=true; command=false;
  }
  if(blockMqtt) now+=20;
}
void bleProcessCommands() {
  if(bleCommand) {
    virtualInputs[0]=true; deadline=now+10;
    mqttFastCommandPending=true; bleCommand=false;
  }
}
void shutterTick() {}
void evalSimpleRules() {
  ++evaluations;
  if(combinedInputs[0] && !prevCombinedInputs[0]) {
    ++rises; toggle=!toggle; output=true;
  }
}
void buildFinalRelays() {} void pcaApplyRelays() {}
void mqttPublishControlStates() {} void ethernetMaintainTick() {} void updateWifiState() {} void heartbeatTick() {} void bleTick() {}
"""
checks = r"""
int main() {
  // A 10 ms command survives 20 ms spent inside MQTT processing.
  command=true; blockMqtt=true; loop();
  assert(output && rises==1 && evaluations==1 && toggle);
  assert(prevCombinedInputs[0] && !mqttFastCommandPending);
  blockMqtt=false; loop(); assert(!combinedInputs[0]);
  // A second press creates exactly one more edge; holding creates none.
  command=true; loop(); assert(rises==2 && !toggle);
  loop(); assert(rises==2 && evaluations==4);
  now+=20; loop(); assert(!combinedInputs[0]);
  // BLE pulses are consumed by the same input evaluation, exactly once.
  bleCommand=true; loop(); assert(rises==3 && toggle);
  loop(); assert(rises==3);
  now+=20; loop(); assert(!combinedInputs[0]);
  // Physical OR virtual: an already active physical input masks the edge.
  inputs[0]=true; loop(); int before=rises;
  command=true; loop(); assert(rises==before);
}
"""
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "input_loop.cpp"
    binary = Path(directory) / "input_loop"
    cpp.write_text(stubs + "\nvoid loop() {\n" + loop + "\n}\n" + checks)
    subprocess.run(["c++", "-std=c++11", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Input loop regression checks passed.")
