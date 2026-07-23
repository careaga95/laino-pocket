#include "PocketSnapshotParser.h"

#include <ArduinoJson.h>

#include <cstring>
#include <memory>
#include <new>
#include <utility>

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

struct TodayContract {
  char nextMeetingId[MAX_SNAPSHOT_ID_BYTES + 1]{};
  char priorityIds[MAX_TODAY_PRIORITIES][MAX_SNAPSHOT_ID_BYTES + 1]{};
  uint32_t counts[MAX_SNAPSHOT_SECTIONS]{};
  uint8_t priorityCount = 0;
  bool hasNextMeeting = false;
};

SnapshotParseResult parseToday(JsonVariantConst value, TodayContract& today) {
  if (!value.is<JsonObjectConst>()) return SnapshotParseResult::MissingRequiredField;
  JsonObjectConst object = value.as<JsonObjectConst>();

  JsonVariantConst nextMeeting = object["nextMeeting"];
  if (!nextMeeting.isNull()) {
    if (!nextMeeting.is<JsonObjectConst>()) return SnapshotParseResult::WrongFieldType;
    const SnapshotParseResult result = copyText(nextMeeting.as<JsonObjectConst>()["id"], today.nextMeetingId);
    if (result != SnapshotParseResult::Success) return result;
    today.hasNextMeeting = true;
  }

  JsonVariantConst prioritiesValue = object["priorities"];
  if (!prioritiesValue.is<JsonArrayConst>()) return SnapshotParseResult::MissingRequiredField;
  JsonArrayConst priorities = prioritiesValue.as<JsonArrayConst>();
  if (priorities.size() > MAX_TODAY_PRIORITIES) return SnapshotParseResult::TooManyItems;
  for (JsonVariantConst priority : priorities) {
    if (!priority.is<JsonObjectConst>()) return SnapshotParseResult::WrongFieldType;
    const SnapshotParseResult result =
        copyText(priority.as<JsonObjectConst>()["id"], today.priorityIds[today.priorityCount]);
    if (result != SnapshotParseResult::Success) return result;
    ++today.priorityCount;
  }

  JsonVariantConst countsValue = object["counts"];
  if (!countsValue.is<JsonObjectConst>()) return SnapshotParseResult::MissingRequiredField;
  JsonObjectConst counts = countsValue.as<JsonObjectConst>();
  constexpr const char* keys[MAX_SNAPSHOT_SECTIONS] = {"agenda", "tasks", "waiting", "owe"};
  for (size_t index = 0; index < MAX_SNAPSHOT_SECTIONS; ++index) {
    if (!counts[keys[index]].is<unsigned>()) return SnapshotParseResult::MissingRequiredField;
    today.counts[index] = counts[keys[index]].as<unsigned>();
  }
  return SnapshotParseResult::Success;
}

uint8_t findItemIndex(const PocketSnapshot& snapshot, const SnapshotSectionId sectionId, const char* id) {
  for (size_t sectionIndex = 0; sectionIndex < snapshot.sectionCount; ++sectionIndex) {
    const SnapshotSection& section = snapshot.sections[sectionIndex];
    if (section.id != sectionId) continue;
    for (size_t itemIndex = 0; itemIndex < section.itemCount; ++itemIndex) {
      const size_t absolute = static_cast<size_t>(section.firstItem) + itemIndex;
      if (absolute < snapshot.itemCount && std::strcmp(snapshot.items[absolute].id, id) == 0) {
        return static_cast<uint8_t>(absolute);
      }
    }
  }
  return INVALID_SNAPSHOT_ITEM_INDEX;
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

  PocketSnapshot parsed;
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

  TodayContract today;
  result = parseToday(root["today"], today);
  if (result != SnapshotParseResult::Success) return result;

  JsonVariantConst sectionsValue = root["sections"];
  if (!sectionsValue.is<JsonArrayConst>()) return SnapshotParseResult::MissingRequiredField;
  JsonArrayConst sections = sectionsValue.as<JsonArrayConst>();
  if (sections.size() == 0 || sections.size() > MAX_SNAPSHOT_SECTIONS) return SnapshotParseResult::TooManySections;

  size_t totalItems = 0;
  uint8_t seen = 0;
  for (JsonVariantConst sectionValue : sections) {
    if (!sectionValue.is<JsonObjectConst>()) return SnapshotParseResult::WrongFieldType;
    JsonObjectConst source = sectionValue.as<JsonObjectConst>();
    if (!source["id"].is<const char*>()) return SnapshotParseResult::MissingRequiredField;
    SnapshotSectionId id;
    size_t maximum = 0;
    if (!sectionId(source["id"].as<const char*>(), id, maximum)) return SnapshotParseResult::InvalidValue;
    const uint8_t bit = static_cast<uint8_t>(1U << static_cast<uint8_t>(id));
    if ((seen & bit) != 0) return SnapshotParseResult::DuplicateSection;
    seen |= bit;
    if (!source["total"].is<unsigned>()) return SnapshotParseResult::MissingRequiredField;
    const unsigned total = source["total"].as<unsigned>();
    JsonVariantConst itemsValue = source["items"];
    if (!itemsValue.is<JsonArrayConst>()) return SnapshotParseResult::MissingRequiredField;
    JsonArrayConst items = itemsValue.as<JsonArrayConst>();
    if (items.size() > maximum || totalItems + items.size() > MAX_SNAPSHOT_ITEMS || total < items.size()) {
      return SnapshotParseResult::TooManyItems;
    }
    totalItems += items.size();
  }

  if (totalItems > 0) {
    parsed.items.reset(new (std::nothrow) SnapshotItem[totalItems]);
    if (!parsed.items) return SnapshotParseResult::OutOfMemory;
  }

  for (JsonVariantConst sectionValue : sections) {
    JsonObjectConst source = sectionValue.as<JsonObjectConst>();
    SnapshotSection& section = parsed.sections[parsed.sectionCount];
    size_t maximum = 0;
    sectionId(source["id"].as<const char*>(), section.id, maximum);
    result = copyText(source["label"], section.label);
    if (result != SnapshotParseResult::Success) return result;
    section.total = source["total"].as<unsigned>();
    JsonArrayConst items = source["items"].as<JsonArrayConst>();
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

  for (size_t sectionIndex = 0; sectionIndex < parsed.sectionCount; ++sectionIndex) {
    const SnapshotSection& section = parsed.sections[sectionIndex];
    if (section.total != today.counts[static_cast<size_t>(section.id)]) {
      return SnapshotParseResult::InvalidValue;
    }
  }
  if (today.hasNextMeeting) {
    parsed.nextMeetingIndex = findItemIndex(parsed, SnapshotSectionId::Agenda, today.nextMeetingId);
    if (parsed.nextMeetingIndex == INVALID_SNAPSHOT_ITEM_INDEX) return SnapshotParseResult::InvalidValue;
  }
  for (size_t index = 0; index < today.priorityCount; ++index) {
    const uint8_t itemIndex = findItemIndex(parsed, SnapshotSectionId::Tasks, today.priorityIds[index]);
    if (itemIndex == INVALID_SNAPSHOT_ITEM_INDEX) return SnapshotParseResult::InvalidValue;
    parsed.priorityIndices[parsed.priorityCount++] = itemIndex;
  }

  parsed.fixture = false;
  destination = std::move(parsed);
  return SnapshotParseResult::Success;
}

SnapshotParseResult validatePocketSnapshot(const char* json, const size_t jsonLength) {
  PocketSnapshot parsed;
  return parsePocketSnapshot(json, jsonLength, parsed);
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
