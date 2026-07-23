#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pocket {

inline constexpr unsigned SNAPSHOT_PROTOCOL_VERSION = 2;
inline constexpr size_t MAX_SNAPSHOT_JSON_BYTES = 16384;
inline constexpr size_t MAX_SNAPSHOT_ITEMS = 56;
inline constexpr size_t MAX_SNAPSHOT_SECTIONS = 4;
inline constexpr size_t MAX_SNAPSHOT_ID_BYTES = 24;
inline constexpr size_t MAX_SNAPSHOT_TITLE_BYTES = 72;
inline constexpr size_t MAX_SNAPSHOT_SUBTITLE_BYTES = 96;
inline constexpr size_t MAX_SNAPSHOT_DETAIL_LINES = 3;
inline constexpr size_t MAX_SNAPSHOT_DETAIL_BYTES = 128;
inline constexpr size_t MAX_TODAY_PRIORITIES = 3;
inline constexpr uint8_t INVALID_SNAPSHOT_ITEM_INDEX = UINT8_MAX;

enum class SnapshotSourceState : uint8_t { Fresh, Partial, Unavailable };
enum class SnapshotSectionId : uint8_t { Agenda, Tasks, Waiting, Owe };

struct SnapshotItem {
  char id[MAX_SNAPSHOT_ID_BYTES + 1]{};
  char title[MAX_SNAPSHOT_TITLE_BYTES + 1]{};
  char subtitle[MAX_SNAPSHOT_SUBTITLE_BYTES + 1]{};
  char detail[MAX_SNAPSHOT_DETAIL_LINES][MAX_SNAPSHOT_DETAIL_BYTES + 1]{};
  uint8_t detailCount = 0;
};

struct SnapshotSection {
  SnapshotSectionId id = SnapshotSectionId::Agenda;
  char label[24]{};
  uint8_t firstItem = 0;
  uint8_t itemCount = 0;
  uint32_t total = 0;
};

struct PocketSnapshot {
  char snapshotId[MAX_SNAPSHOT_ID_BYTES + 1]{};
  uint64_t generatedAtEpoch = 0;
  uint64_t refreshAfterEpoch = 0;
  SnapshotSourceState calendarState = SnapshotSourceState::Unavailable;
  SnapshotSourceState tasksState = SnapshotSourceState::Unavailable;
  SnapshotSourceState commitmentsState = SnapshotSourceState::Unavailable;
  SnapshotSection sections[MAX_SNAPSHOT_SECTIONS]{};
  std::unique_ptr<SnapshotItem[]> items;
  uint8_t nextMeetingIndex = INVALID_SNAPSHOT_ITEM_INDEX;
  uint8_t priorityIndices[MAX_TODAY_PRIORITIES] = {INVALID_SNAPSHOT_ITEM_INDEX, INVALID_SNAPSHOT_ITEM_INDEX,
                                                   INVALID_SNAPSHOT_ITEM_INDEX};
  uint8_t sectionCount = 0;
  uint8_t itemCount = 0;
  uint8_t priorityCount = 0;
  bool fixture = false;

  [[nodiscard]] const SnapshotSection* sectionAt(const size_t index) const {
    return index < sectionCount ? &sections[index] : nullptr;
  }

  [[nodiscard]] const SnapshotItem* itemAt(const SnapshotSection& section, const size_t index) const {
    const size_t absolute = static_cast<size_t>(section.firstItem) + index;
    return items && index < section.itemCount && absolute < itemCount ? &items[absolute] : nullptr;
  }

  [[nodiscard]] const SnapshotItem* nextMeeting() const {
    return items && nextMeetingIndex < itemCount ? &items[nextMeetingIndex] : nullptr;
  }

  [[nodiscard]] const SnapshotItem* priorityAt(const size_t index) const {
    if (!items || index >= priorityCount) return nullptr;
    const uint8_t absolute = priorityIndices[index];
    return absolute < itemCount ? &items[absolute] : nullptr;
  }
};

static_assert(sizeof(PocketSnapshot) <= 256, "Pocket snapshot metadata RAM budget exceeded");
static_assert(sizeof(SnapshotItem) * MAX_SNAPSHOT_ITEMS <= 35000, "Pocket snapshot item RAM budget exceeded");

void loadEmptySnapshot(PocketSnapshot& destination);
bool snapshotHasPartialSource(const PocketSnapshot& snapshot);

}  // namespace pocket
