#pragma once

#include <cstddef>
#include <cstdint>

#include "PocketPairingProtocol.h"

#ifndef POCKET_FIRMWARE_ID
#error "POCKET_FIRMWARE_ID must be supplied by the build"
#endif

namespace pocket {

inline constexpr char POCKET_DEVICE_MODEL[] = "xteink-x3";
inline constexpr char POCKET_FIRMWARE_BUILD_ID[] = POCKET_FIRMWARE_ID;

struct PairingIdentity {
  uint8_t protocol;
  const char* model;
  const char* firmware;
};

constexpr bool isPocketBuildIdentifierCharacter(const char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9') || value == '.' || value == '+' || value == '-' || value == '_';
}

template <std::size_t Size>
constexpr bool validPocketBuildIdentifier(const char (&value)[Size]) {
  if (Size <= 1 || Size - 1 > 32) return false;
  for (std::size_t index = 0; index + 1 < Size; ++index) {
    if (!isPocketBuildIdentifierCharacter(value[index])) return false;
  }
  return value[Size - 1] == '\0';
}

static_assert(sizeof(POCKET_FIRMWARE_BUILD_ID) - 1 <= 32,
              "Pocket firmware identifier exceeds the 32-character wire contract");
static_assert(validPocketBuildIdentifier(POCKET_FIRMWARE_BUILD_ID),
              "Pocket firmware identifier must be non-empty ASCII without spaces");
static_assert(sizeof(POCKET_DEVICE_MODEL) - 1 <= 32, "Pocket model exceeds the wire contract");

inline constexpr PairingIdentity COMPILED_PAIRING_IDENTITY{
    PAIRING_PROTOCOL_VERSION,
    POCKET_DEVICE_MODEL,
    POCKET_FIRMWARE_BUILD_ID,
};

}  // namespace pocket
