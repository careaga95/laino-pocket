#include "PocketReading.h"

#include <cstdio>
#include <cstring>

namespace pocket {

void loadEmptyReadingManifest(ReadingManifest& destination) { destination = {}; }

bool isReadingUuid(const char* value) {
  if (value == nullptr || std::strlen(value) != MAX_READING_ID_BYTES) return false;
  for (size_t index = 0; index < MAX_READING_ID_BYTES; ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
      continue;
    }
    const char c = value[index];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return value[14] == '4' && (value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}

namespace {

bool formatPath(const char* format, const ReadingItem& item, char* destination, const size_t capacity) {
  if (destination == nullptr || capacity == 0 || !isReadingUuid(item.id)) return false;
  const int length = std::snprintf(destination, capacity, format, READING_DIRECTORY, item.id);
  return length > 0 && static_cast<size_t>(length) < capacity;
}

}  // namespace

bool readingPath(const ReadingItem& item, char* destination, const size_t capacity) {
  return formatPath("%s/%s.epub", item, destination, capacity);
}

bool readingPartPath(const ReadingItem& item, char* destination, const size_t capacity) {
  return formatPath("%s/.download-%s.part", item, destination, capacity);
}

}  // namespace pocket
