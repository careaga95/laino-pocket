#pragma once

#include "PocketBundleCache.h"
#include "PocketSnapshot.h"

namespace pocket {

inline constexpr char SNAPSHOT_SLOT_A_PATH[] = "/laino/pocket/snapshot-a.bin";
inline constexpr char SNAPSHOT_SLOT_B_PATH[] = "/laino/pocket/snapshot-b.bin";

class PocketSnapshotCache final {
  PocketCacheStorage& storage;
  char jsonBuffer[MAX_SNAPSHOT_JSON_BYTES + 1]{};

  CacheOutcome inspect(CacheSlot slot, size_t* payloadLength = nullptr);

 public:
  explicit PocketSnapshotCache(PocketCacheStorage& storage) : storage(storage) {}

  CacheOutcome loadBest(PocketSnapshot& destination);
  CacheOutcome store(const char* json, size_t jsonLength);
};

}  // namespace pocket
