"""Host regression of the actual MQTT cleanup/subscription functions."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / "src/main.cpp").read_text()
functions = source.split("static bool mqttClearRetainedCommands", 1)[1].split(
    "static String mqttClientIdForTransport", 1)[0]
functions = "static bool mqttClearRetainedCommands" + functions
harness = r"""
#include <cassert>
#include <map>
#include <string>
#include <vector>
struct String {
  std::string value;
  String(const char* s): value(s) {}
  String(int n): value(std::to_string(n)) {}
  String(std::string s): value(s) {}
  const char* c_str() const { return value.c_str(); }
  String operator+(const String& other) const { return String(value+other.value); }
};
int totalRelays=4, totalInputs=4;
int shuttersLimit() { return 2; }
String mqttBaseTopic() { return String("board"); }
struct PubSubClient {
  std::map<std::string,std::string> retained;
  std::vector<std::string> replayed, subscribed;
  int count=0, failAt=-1;
  bool publish(const char* topic, const char* payload, bool retain) {
    assert(retain && std::string(payload).empty());
    if(++count==failAt) return false;
    retained.erase(topic);
    return true;
  }
  bool subscribe(const char* topic) {
    subscribed.push_back(topic);
    if(retained.count(topic)) replayed.push_back(retained[topic]);
    return true;
  }
};
"""
checks = r"""
int main() {
  PubSubClient client;
  client.retained["board/relay/1/set"]="ON";
  client.retained["board/relay/2/set"]="ON";
  client.retained["board/vin/1/set"]="TOGGLE";
  client.retained["board/shutter/1/set"]="OPEN";
  client.retained["board/relay/1/state"]="ON";
  assert(mqttClearRetainedCommands(client));
  mqttSubscribeTopics(client);
  assert(client.replayed.empty());
  assert(client.subscribed.size()==16);
  assert(client.retained.size()==1);
  assert(client.retained["board/relay/1/state"]=="ON");
  PubSubClient failure;
  failure.failAt=2;
  assert(!mqttClearRetainedCommands(failure));
  assert(failure.subscribed.empty());
}
"""
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "mqtt_reconnect.cpp"
    binary = Path(directory) / "mqtt_reconnect"
    cpp.write_text(harness + functions + checks)
    subprocess.run(["c++", "-std=c++11", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("MQTT reconnect regression checks passed.")
