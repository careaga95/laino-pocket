#pragma once

#include "PocketBundleCache.h"

namespace pocket {

class PocketSdCacheStorage final : public PocketCacheStorage {
 public:
  bool available() const override;
  bool ensureDirectory(const char* path) override;
  CacheResult read(const char* path, uint8_t* header, size_t headerCapacity, size_t& headerRead, char* payload,
                   size_t payloadCapacity, size_t& payloadRead, size_t& fileSize) override;
  CacheResult write(const char* path, const uint8_t* header, size_t headerLength, const char* payload,
                    size_t payloadLength) override;
};

}  // namespace pocket
