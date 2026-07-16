#pragma once

#include <cstddef>
#include <cstdint>

#include "PocketCard.h"

namespace pocket {

inline constexpr size_t CACHE_HEADER_BYTES = 28;
inline constexpr uint16_t CACHE_FORMAT_VERSION = 1;
inline constexpr uint16_t CACHE_FLAGS = 0;
inline constexpr char CACHE_DIRECTORY[] = "/laino/pocket";
inline constexpr char CACHE_SLOT_A_PATH[] = "/laino/pocket/cards-a.bin";
inline constexpr char CACHE_SLOT_B_PATH[] = "/laino/pocket/cards-b.bin";

enum class CacheResult : uint8_t {
  Success,
  NoValidSlot,
  SdUnavailable,
  FileNotFound,
  ReadFailure,
  WriteFailure,
  InvalidHeader,
  UnsupportedCacheFormat,
  InvalidLength,
  ChecksumMismatch,
  InvalidJsonBundle,
  VerificationFailure,
  GenerationExhausted,
};

enum class CacheSlot : uint8_t { None, A, B };

struct CacheOutcome {
  CacheResult result = CacheResult::NoValidSlot;
  CacheSlot slot = CacheSlot::None;
  uint64_t generation = 0;
};

class PocketCacheStorage {
 public:
  virtual ~PocketCacheStorage() = default;
  virtual bool available() const = 0;
  virtual bool ensureDirectory(const char* path) = 0;
  virtual CacheResult read(const char* path, uint8_t* header, size_t headerCapacity, size_t& headerRead, char* payload,
                           size_t payloadCapacity, size_t& payloadRead, size_t& fileSize) = 0;
  virtual CacheResult write(const char* path, const uint8_t* header, size_t headerLength, const char* payload,
                            size_t payloadLength) = 0;
};

uint32_t pocketCacheCrc32(const void* data, size_t length, uint32_t previous = 0);
CacheResult decodePocketCacheSlot(const uint8_t* header, size_t headerBytes, const char* payload, size_t payloadBytes,
                                  size_t fileSize, uint64_t& generation);
void encodePocketCacheHeader(uint8_t (&header)[CACHE_HEADER_BYTES], uint64_t generation, const char* payload,
                             size_t payloadLength);
const char* cacheResultName(CacheResult result);
const char* cacheSlotName(CacheSlot slot);

class PocketBundleCache final {
  PocketCacheStorage& storage;
  char jsonBuffer[MAX_JSON_DOCUMENT_BYTES + 1]{};

  CacheOutcome inspect(CacheSlot slot, size_t* payloadLength = nullptr);

 public:
  explicit PocketBundleCache(PocketCacheStorage& storage) : storage(storage) {}

  CacheOutcome loadBest(CardBundle& destination);
  CacheOutcome store(const char* json, size_t jsonLength);
};

}  // namespace pocket
