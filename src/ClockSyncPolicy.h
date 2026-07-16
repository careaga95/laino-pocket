#pragma once

#include <cstdint>

namespace ClockSyncPolicy {

inline constexpr uint8_t COMPLETE_DATE_TIME_VERSION = 1;

constexpr bool shouldAutoSync(const bool legacySyncComplete, const uint8_t completedDataVersion) {
  return !legacySyncComplete || completedDataVersion < COMPLETE_DATE_TIME_VERSION;
}

constexpr uint8_t markComplete(const uint8_t completedDataVersion) {
  return completedDataVersion < COMPLETE_DATE_TIME_VERSION ? COMPLETE_DATE_TIME_VERSION : completedDataVersion;
}

}  // namespace ClockSyncPolicy
