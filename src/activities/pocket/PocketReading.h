#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pocket {

inline constexpr unsigned READING_PROTOCOL_VERSION = 1;
inline constexpr size_t MAX_READING_MANIFEST_JSON_BYTES = 8192;
inline constexpr size_t MAX_READING_ITEMS = 20;
inline constexpr size_t MAX_READING_ID_BYTES = 36;
inline constexpr size_t MAX_READING_TITLE_BYTES = 448;
inline constexpr size_t MAX_READING_BYLINE_BYTES = 256;
inline constexpr size_t READING_SHA256_HEX_BYTES = 64;
inline constexpr uint32_t MAX_READING_EPUB_BYTES = 8U * 1024U * 1024U;
inline constexpr char READING_DIRECTORY[] = "/Hari/Reading";

struct ReadingItem {
  char id[MAX_READING_ID_BYTES + 1]{};
  char title[MAX_READING_TITLE_BYTES + 1]{};
  char byline[MAX_READING_BYLINE_BYTES + 1]{};
  char sha256[READING_SHA256_HEX_BYTES + 1]{};
  uint64_t queuedAtEpoch = 0;
  uint32_t bytes = 0;
};

struct ReadingManifest {
  uint64_t generatedAtEpoch = 0;
  std::unique_ptr<ReadingItem[]> items;
  uint8_t itemCount = 0;

  [[nodiscard]] const ReadingItem* itemAt(const size_t index) const {
    return items && index < itemCount ? &items[index] : nullptr;
  }
};

static_assert(sizeof(ReadingItem) * MAX_READING_ITEMS <= 17000, "Pocket reading RAM budget exceeded");

void loadEmptyReadingManifest(ReadingManifest& destination);
bool isReadingUuid(const char* value);
bool readingPath(const ReadingItem& item, char* destination, size_t capacity);
bool readingPartPath(const ReadingItem& item, char* destination, size_t capacity);

}  // namespace pocket
