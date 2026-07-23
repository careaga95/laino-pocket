#include "PocketReadingParser.h"

#include <ArduinoJson.h>

#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace pocket {
namespace {

template <size_t Capacity>
ReadingParseResult copyText(JsonVariantConst value, char (&destination)[Capacity], const bool required = true) {
  if (value.isNull()) return required ? ReadingParseResult::MissingRequiredField : ReadingParseResult::Success;
  if (!value.is<const char*>()) return ReadingParseResult::WrongFieldType;
  const char* source = value.as<const char*>();
  const size_t length = std::strlen(source);
  if (length >= Capacity) return ReadingParseResult::TextTooLong;
  if (required && length == 0) return ReadingParseResult::InvalidValue;
  std::memcpy(destination, source, length + 1);
  return ReadingParseResult::Success;
}

bool isLowerHexDigest(const char* value) {
  if (value == nullptr || std::strlen(value) != READING_SHA256_HEX_BYTES) return false;
  for (size_t index = 0; index < READING_SHA256_HEX_BYTES; ++index) {
    const char c = value[index];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool isValidDisplayUtf8(const char* value) {
  if (value == nullptr) return false;
  const auto* cursor = reinterpret_cast<const uint8_t*>(value);
  const auto* end = cursor + std::strlen(value);
  while (cursor < end) {
    const uint8_t first = *cursor++;
    if (first <= 0x7f) {
      if (first < 0x20 || first == 0x7f) return false;
      continue;
    }
    uint32_t codePoint = 0;
    uint8_t continuationCount = 0;
    uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      codePoint = first & 0x1f;
      continuationCount = 1;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      codePoint = first & 0x0f;
      continuationCount = 2;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      codePoint = first & 0x07;
      continuationCount = 3;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (static_cast<size_t>(end - cursor) < continuationCount) return false;
    for (uint8_t index = 0; index < continuationCount; ++index) {
      const uint8_t next = *cursor++;
      if ((next & 0xc0) != 0x80) return false;
      codePoint = (codePoint << 6) | (next & 0x3f);
    }
    if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) return false;
  }
  return true;
}

ReadingParseResult parseItem(JsonObjectConst source, ReadingItem& item) {
  ReadingParseResult result = copyText(source["id"], item.id);
  if (result != ReadingParseResult::Success) return result;
  result = copyText(source["title"], item.title);
  if (result != ReadingParseResult::Success) return result;
  result = copyText(source["byline"], item.byline, false);
  if (result != ReadingParseResult::Success) return result;
  result = copyText(source["sha256"], item.sha256);
  if (result != ReadingParseResult::Success) return result;
  if (!source["queuedAtEpoch"].is<uint64_t>() || !source["bytes"].is<uint32_t>() ||
      !source["format"].is<const char*>()) {
    return ReadingParseResult::MissingRequiredField;
  }
  item.queuedAtEpoch = source["queuedAtEpoch"].as<uint64_t>();
  item.bytes = source["bytes"].as<uint32_t>();
  if (!isReadingUuid(item.id) || !isValidDisplayUtf8(item.title) ||
      (item.byline[0] != '\0' && !isValidDisplayUtf8(item.byline)) || !isLowerHexDigest(item.sha256) ||
      item.queuedAtEpoch == 0 || item.bytes == 0 ||
      item.bytes > MAX_READING_EPUB_BYTES || std::strcmp(source["format"].as<const char*>(), "epub") != 0) {
    return ReadingParseResult::InvalidValue;
  }
  return ReadingParseResult::Success;
}

}  // namespace

ReadingParseResult parseReadingManifest(const char* json, const size_t jsonLength, ReadingManifest& destination) {
  if (json == nullptr || jsonLength == 0) return ReadingParseResult::EmptyInput;
  if (jsonLength > MAX_READING_MANIFEST_JSON_BYTES) return ReadingParseResult::DocumentTooLarge;

  JsonDocument document;
  if (deserializeJson(document, json, jsonLength)) return ReadingParseResult::MalformedJson;
  JsonObjectConst root = document.as<JsonObjectConst>();
  if (root.isNull()) return ReadingParseResult::WrongFieldType;
  if (!root["protocolVersion"].is<unsigned>()) return ReadingParseResult::MissingRequiredField;
  if (root["protocolVersion"].as<unsigned>() != READING_PROTOCOL_VERSION) {
    return ReadingParseResult::UnsupportedProtocolVersion;
  }
  if (!root["generatedAtEpoch"].is<uint64_t>()) return ReadingParseResult::MissingRequiredField;
  JsonVariantConst itemsValue = root["items"];
  if (!itemsValue.is<JsonArrayConst>()) return ReadingParseResult::MissingRequiredField;
  JsonArrayConst items = itemsValue.as<JsonArrayConst>();
  if (items.size() > MAX_READING_ITEMS) return ReadingParseResult::TooManyItems;

  ReadingManifest parsed;
  parsed.generatedAtEpoch = root["generatedAtEpoch"].as<uint64_t>();
  if (parsed.generatedAtEpoch == 0) return ReadingParseResult::InvalidValue;
  if (items.size() > 0) {
    parsed.items.reset(new (std::nothrow) ReadingItem[items.size()]);
    if (!parsed.items) return ReadingParseResult::OutOfMemory;
  }
  for (JsonVariantConst value : items) {
    if (!value.is<JsonObjectConst>()) return ReadingParseResult::WrongFieldType;
    ReadingParseResult result = parseItem(value.as<JsonObjectConst>(), parsed.items[parsed.itemCount]);
    if (result != ReadingParseResult::Success) return result;
    for (size_t previous = 0; previous < parsed.itemCount; ++previous) {
      if (std::strcmp(parsed.items[previous].id, parsed.items[parsed.itemCount].id) == 0) {
        return ReadingParseResult::DuplicateItem;
      }
    }
    ++parsed.itemCount;
  }
  destination = std::move(parsed);
  return ReadingParseResult::Success;
}

ReadingParseResult validateReadingManifest(const char* json, const size_t jsonLength) {
  ReadingManifest parsed;
  return parseReadingManifest(json, jsonLength, parsed);
}

const char* readingParseResultName(const ReadingParseResult result) {
  switch (result) {
    case ReadingParseResult::Success:
      return "success";
    case ReadingParseResult::EmptyInput:
      return "empty_input";
    case ReadingParseResult::DocumentTooLarge:
      return "document_too_large";
    case ReadingParseResult::MalformedJson:
      return "malformed_json";
    case ReadingParseResult::UnsupportedProtocolVersion:
      return "unsupported_protocol";
    case ReadingParseResult::MissingRequiredField:
      return "missing_field";
    case ReadingParseResult::WrongFieldType:
      return "wrong_type";
    case ReadingParseResult::TooManyItems:
      return "too_many_items";
    case ReadingParseResult::TextTooLong:
      return "text_too_long";
    case ReadingParseResult::InvalidValue:
      return "invalid_value";
    case ReadingParseResult::DuplicateItem:
      return "duplicate_item";
    case ReadingParseResult::OutOfMemory:
      return "out_of_memory";
  }
  return "unknown";
}

}  // namespace pocket
