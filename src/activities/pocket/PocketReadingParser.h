#pragma once

#include <cstddef>
#include <cstdint>

#include "PocketReading.h"

namespace pocket {

enum class ReadingParseResult : uint8_t {
  Success,
  EmptyInput,
  DocumentTooLarge,
  MalformedJson,
  UnsupportedProtocolVersion,
  MissingRequiredField,
  WrongFieldType,
  TooManyItems,
  TextTooLong,
  InvalidValue,
  DuplicateItem,
  OutOfMemory,
};

ReadingParseResult parseReadingManifest(const char* json, size_t jsonLength, ReadingManifest& destination);
ReadingParseResult validateReadingManifest(const char* json, size_t jsonLength);
const char* readingParseResultName(ReadingParseResult result);

}  // namespace pocket
