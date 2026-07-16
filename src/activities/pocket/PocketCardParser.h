#pragma once

#include <cstddef>
#include <cstdint>

#include "PocketCard.h"

namespace pocket {

enum class ParseResult : uint8_t {
  Success,
  EmptyInput,
  DocumentTooLarge,
  MalformedJson,
  UnsupportedProtocolVersion,
  MissingRequiredField,
  WrongFieldType,
  DuplicateField,
  EmptyCards,
  TooManyCards,
  TooManyLines,
  TextTooLong,
  InvalidUtf8,
};

ParseResult parseCardBundle(const char* json, size_t jsonLength, CardBundle& destination);
const char* parseResultName(ParseResult result);

}  // namespace pocket
