"""Exercise the actual status LED timer, including repeated requests and rollover."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / 'src/main.cpp').read_text()
start = source.index('static bool identificationActive')
end = source.index('static void logFactoryPinState', start)
code = r'''
#include "control_time.h"
#include <cassert>
#include <cstdint>
uint32_t now=0;
uint32_t millis() { return now; }
constexpr int PIN_LED=40;
int writes=0, led=-1;
void digitalWrite(int pin, int state) { assert(pin==PIN_LED); ++writes; led=state; }
'''
code += source[start:end]
code += r'''
int main() {
  heartbeatTick(); assert(writes==0);
  now=999; heartbeatTick(); assert(writes==0);
  now=1000; heartbeatTick(); assert(writes==1);
  now=1050; startIdentification();
  heartbeatTick(); assert(writes==1);
  now=1099; heartbeatTick(); assert(writes==1);
  now=1100; heartbeatTick(); assert(writes==2);
  now=1200; heartbeatTick(); assert(writes==3);
  now=10000; startIdentification(); // Restart the full ten seconds.
  now=11050; heartbeatTick(); assert(identificationActive);
  now=19999; heartbeatTick(); assert(identificationActive);
  now=20000; heartbeatTick(); assert(!identificationActive);
  int before=writes;
  now=20099; heartbeatTick(); assert(writes==before);
  now=20999; heartbeatTick(); assert(writes==before+1);
  // millis() wraps during identification; the timer remains active until deadline.
  now=UINT32_MAX-5000; startIdentification(); heartbeatTick();
  now=0; heartbeatTick(); assert(identificationActive);
  now=4998; heartbeatTick(); assert(identificationActive);
  now=4999; heartbeatTick(); assert(!identificationActive);
  // A deadline of zero is valid too.
  now=UINT32_MAX-9999; startIdentification();
  assert(identificationUntilMs==0);
  now=UINT32_MAX; heartbeatTick(); assert(identificationActive);
  now=0; heartbeatTick(); assert(!identificationActive);
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'identification.cpp'
    binary = Path(directory) / 'identification'
    cpp.write_text(code)
    subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(project / 'include'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Identification LED: fast blinking, expiry, restart and rollover OK')
