#pragma once

#include <cstddef>
#include <cstdint>

#include "PocketSnapshot.h"

namespace pocket {

enum class SnapshotParseResult : uint8_t {
  Success,
  EmptyInput,
  DocumentTooLarge,
  MalformedJson,
  UnsupportedProtocolVersion,
  MissingRequiredField,
  WrongFieldType,
  DuplicateSection,
  TooManySections,
  TooManyItems,
  TextTooLong,
  InvalidValue,
  OutOfMemory,
};

SnapshotParseResult parsePocketSnapshot(const char* json, size_t jsonLength, PocketSnapshot& destination);
SnapshotParseResult validatePocketSnapshot(const char* json, size_t jsonLength);
const char* snapshotParseResultName(SnapshotParseResult result);

}  // namespace pocket
