#pragma once
#include <stdint.h>

// Deadlines are scheduled less than INT32_MAX milliseconds into the future.
// Keep a separate active flag; zero is a valid deadline after rollover.
static inline bool deadlineReached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}
