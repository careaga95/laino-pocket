#pragma once

#include "PocketBundleCache.h"
#include "PocketReading.h"

namespace pocket {

inline constexpr char READING_SLOT_A_PATH[] = "/laino/pocket/reading-a.bin";
inline constexpr char READING_SLOT_B_PATH[] = "/laino/pocket/reading-b.bin";

class PocketReadingCache final {
  PocketCacheStorage& storage;

  CacheOutcome inspect(CacheSlot slot, char* jsonBuffer, size_t jsonCapacity, size_t* payloadLength = nullptr);

 public:
  explicit PocketReadingCache(PocketCacheStorage& storage) : storage(storage) {}

  CacheOutcome loadBest(ReadingManifest& destination);
  CacheOutcome store(const char* json, size_t jsonLength);
};

}  // namespace pocket
