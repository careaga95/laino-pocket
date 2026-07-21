#include "PocketSnapshotParser.h"

#include <ArduinoJson.h>

#include <cstring>
#include <memory>
#include <new>

namespace pocket {
namespace {

template <size_t Capacity>
SnapshotParseResult copyText(JsonVariantConst value, char (&destination)[Capacity], const bool required = true) {
  if (value.isNull()) return required ? SnapshotParseResult::MissingRequiredField : SnapshotParseResult::Success;
  if (!value.is<const char*>()) return SnapshotParseResult::WrongFieldType;
  const char* source = value.as<const char*>();
  const size_t length = std::strlen(source);
  if (length >= Capacity) return SnapshotParseResult::TextTooLong;
  std::memcpy(destination, source, length + 1);
  return SnapshotParseResult::Success;
}

bool sourceState(JsonVariantConst value, SnapshotSourceState& state) {
  if (!value.is<const char*>()) return false;
  const char* text = value.as<const char*>();
  if (std::strcmp(text, "fresh") == 0)
    state = SnapshotSourceState::Fresh;
  else if (std::strcmp(text, "partial") == 0)
    state = SnapshotSourceState::Partial;
  else if (std::strcmp(text, "unavailable") == 0)
    state = SnapshotSourceState::Unavailable;
  else
    return false;
  return true;
}

bool sectionId(const char* value, SnapshotSectionId& id, size_t& maximum) {
  if (std::strcmp(value, "agenda") == 0) {
    id = SnapshotSectionId::Agenda;
    maximum = 12;
  } else if (std::strcmp(value, "tasks") == 0) {
    id = SnapshotSectionId::Tasks;
    maximum = 20;
  } else if (std::strcmp(value, "waiting") == 0) {
    id = SnapshotSectionId::Waiting;
    maximum = 12;
  } else if (std::strcmp(value, "owe") == 0) {
    id = SnapshotSectionId::Owe;
    maximum = 12;
  } else {
    return false;
  }
  return true;
}

SnapshotParseResult parseItem(JsonObjectConst object, SnapshotItem& item) {
  SnapshotParseResult result = copyText(object["id"], item.id);
  if (result != SnapshotParseResult::Success) return result;
  result = copyText(object["title"], item.title);
  if (result != SnapshotParseResult::Success) return result;
  result = copyText(object["subtitle"], item.subtitle);
  if (result != SnapshotParseResult::Success) return result;
  JsonVariantConst detailValue = object["detail"];
  if (!detailValue.is<JsonArrayConst>()) return SnapshotParseResult::WrongFieldType;
  JsonArrayConst detail = detailValue.as<JsonArrayConst>();
  if (detail.size() > MAX_SNAPSHOT_DETAIL_LINES) return SnapshotParseResult::TooManyItems;
  for (JsonVariantConst line : detail) {
    result = copyText(line, item.detail[item.detailCount]);
    if (result != SnapshotParseResult::Success) return result;
    ++item.detailCount;
  }
  return SnapshotParseResult::Success;
}

}  // namespace

SnapshotParseResult parsePocketSnapshot(const char* json, const size_t jsonLength, PocketSnapshot& destination) {
  if (json == nullptr || jsonLength == 0) return SnapshotParseResult::EmptyInput;
  if (jsonLength > MAX_SNAPSHOT_JSON_BYTES) return SnapshotParseResult::DocumentTooLarge;

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, json, jsonLength);
  if (error) return SnapshotParseResult::MalformedJson;
  JsonObjectConst root = document.as<JsonObjectConst>();
  if (root.isNull()) return SnapshotParseResult::WrongFieldType;
  if (!root["protocolVersion"].is<unsigned>()) return SnapshotParseResult::MissingRequiredField;
  if (root["protocolVersion"].as<unsigned>() != SNAPSHOT_PROTOCOL_VERSION) {
    return SnapshotParseResult::UnsupportedProtocolVersion;
  }

  std::unique_ptr<PocketSnapshot> holder(new (std::nothrow) PocketSnapshot());
  if (!holder) return SnapshotParseResult::OutOfMemory;
  PocketSnapshot& parsed = *holder;
  SnapshotParseResult result = copyText(root["snapshotId"], parsed.snapshotId);
  if (result != SnapshotParseResult::Success) return result;
  if (!root["generatedAtEpoch"].is<uint64_t>() || !root["refreshAfterEpoch"].is<uint64_t>()) {
    return SnapshotParseResult::MissingRequiredField;
  }
  parsed.generatedAtEpoch = root["generatedAtEpoch"].as<uint64_t>();
  parsed.refreshAfterEpoch = root["refreshAfterEpoch"].as<uint64_t>();
  if (parsed.generatedAtEpoch == 0 || parsed.refreshAfterEpoch < parsed.generatedAtEpoch) {
    return SnapshotParseResult::InvalidValue;
  }

  JsonVariantConst statusValue = root["sourceStatus"];
  if (!statusValue.is<JsonObjectConst>()) return SnapshotParseResult::MissingRequiredField;
  JsonObjectConst status = statusValue.as<JsonObjectConst>();
  if (!sourceState(status["calendar"], parsed.calendarState) || !sourceState(status["tasks"], parsed.tasksState) ||
      !sourceState(status["commitments"], parsed.commitmentsState)) {
    return SnapshotParseResult::InvalidValue;
  }

  JsonVariantConst sectionsValue = root["sections"];
  if (!sectionsValue.is<JsonArrayConst>()) return SnapshotParseResult::MissingRequiredField;
  JsonArrayConst sections = sectionsValue.as<JsonArrayConst>();
  if (sections.size() == 0 || sections.size() > MAX_SNAPSHOT_SECTIONS) return SnapshotParseResult::TooManySections;
  uint8_t seen = 0;
  for (JsonVariantConst sectionValue : sections) {
    if (!sectionValue.is<JsonObjectConst>()) return SnapshotParseResult::WrongFieldType;
    JsonObjectConst source = sectionValue.as<JsonObjectConst>();
    if (!source["id"].is<const char*>()) return SnapshotParseResult::MissingRequiredField;
    SnapshotSection& section = parsed.sections[parsed.sectionCount];
    size_t maximum = 0;
    if (!sectionId(source["id"].as<const char*>(), section.id, maximum)) return SnapshotParseResult::InvalidValue;
    const uint8_t bit = static_cast<uint8_t>(1U << static_cast<uint8_t>(section.id));
    if ((seen & bit) != 0) return SnapshotParseResult::DuplicateSection;
    seen |= bit;
    result = copyText(source["label"], section.label);
    if (result != SnapshotParseResult::Success) return result;
    if (!source["total"].is<unsigned>()) return SnapshotParseResult::MissingRequiredField;
    const unsigned total = source["total"].as<unsigned>();
    if (total > UINT8_MAX) return SnapshotParseResult::InvalidValue;
    section.total = static_cast<uint8_t>(total);
    JsonVariantConst itemsValue = source["items"];
    if (!itemsValue.is<JsonArrayConst>()) return SnapshotParseResult::MissingRequiredField;
    JsonArrayConst items = itemsValue.as<JsonArrayConst>();
    if (items.size() > maximum || parsed.itemCount + items.size() > MAX_SNAPSHOT_ITEMS || total < items.size()) {
      return SnapshotParseResult::TooManyItems;
    }
    section.firstItem = parsed.itemCount;
    for (JsonVariantConst itemValue : items) {
      if (!itemValue.is<JsonObjectConst>()) return SnapshotParseResult::WrongFieldType;
      result = parseItem(itemValue.as<JsonObjectConst>(), parsed.items[parsed.itemCount]);
      if (result != SnapshotParseResult::Success) return result;
      ++parsed.itemCount;
      ++section.itemCount;
    }
    ++parsed.sectionCount;
  }
  parsed.fixture = false;
  destination = parsed;
  return SnapshotParseResult::Success;
}

SnapshotParseResult validatePocketSnapshot(const char* json, const size_t jsonLength) {
  std::unique_ptr<PocketSnapshot> parsed(new (std::nothrow) PocketSnapshot());
  return parsed ? parsePocketSnapshot(json, jsonLength, *parsed) : SnapshotParseResult::OutOfMemory;
}

const char* snapshotParseResultName(const SnapshotParseResult result) {
  switch (result) {
    case SnapshotParseResult::Success:
      return "success";
    case SnapshotParseResult::EmptyInput:
      return "empty_input";
    case SnapshotParseResult::DocumentTooLarge:
      return "document_too_large";
    case SnapshotParseResult::MalformedJson:
      return "malformed_json";
    case SnapshotParseResult::UnsupportedProtocolVersion:
      return "unsupported_protocol";
    case SnapshotParseResult::MissingRequiredField:
      return "missing_field";
    case SnapshotParseResult::WrongFieldType:
      return "wrong_type";
    case SnapshotParseResult::DuplicateSection:
      return "duplicate_section";
    case SnapshotParseResult::TooManySections:
      return "too_many_sections";
    case SnapshotParseResult::TooManyItems:
      return "too_many_items";
    case SnapshotParseResult::TextTooLong:
      return "text_too_long";
    case SnapshotParseResult::InvalidValue:
      return "invalid_value";
    case SnapshotParseResult::OutOfMemory:
      return "out_of_memory";
  }
  return "unknown";
}

}  // namespace pocket
