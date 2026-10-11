#pragma once
#include <cstring>

struct BleCommand {
  char target;
  unsigned int index;
  char action[8];
};

// Commands fit in the default ATT payload (20 bytes): R1:ON / V1:TOGGLE.
inline bool parseBleCommand(const char* data, size_t length, BleCommand& command) {
  if (length == 8 && !std::memcmp(data, "IDENTIFY", 8)) {
    command = BleCommand{'I', 0, {0}};
    return true;
  }
  if (length < 5 || length > 12 || (data[0] != 'R' && data[0] != 'V')) return false;
  size_t pos = 1;
  unsigned int index = 0;
  while (pos < length && data[pos] >= '0' && data[pos] <= '9') {
    index = index * 10 + (data[pos++] - '0');
  }
  if (index < 1 || index > 16 || pos >= length || data[pos++] != ':') return false;
  const size_t actionLength = length - pos;
  if (actionLength == 0 || actionLength >= sizeof(command.action)) return false;
  // Reject embedded NULs, suffixes, and commands for unsupported targets.
  if (std::memchr(data + pos, '\0', actionLength)) return false;
  std::memcpy(command.action, data + pos, actionLength);
  command.action[actionLength] = '\0';
  if (std::strcmp(command.action, "ON") && std::strcmp(command.action, "OFF") &&
      std::strcmp(command.action, "TOGGLE") &&
      !(data[0] == 'R' && !std::strcmp(command.action, "AUTO"))) return false;
  command.target = data[0];
  command.index = index;
  return true;
}
