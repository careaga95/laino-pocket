#include "PocketSnapshotCache.h"

#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include "PocketSnapshotParser.h"

namespace pocket {
namespace {

constexpr char CACHE_MAGIC[] = "LPSNAP2";

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t readLe64(const uint8_t* data) {
  uint64_t value = 0;
  for (size_t index = 0; index < 8; ++index) value |= static_cast<uint64_t>(data[index]) << (index * 8U);
  return value;
}

void writeLe16(uint8_t* data, const uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32(uint8_t* data, const uint32_t value) {
  for (size_t index = 0; index < 4; ++index) data[index] = static_cast<uint8_t>(value >> (index * 8U));
}

void writeLe64(uint8_t* data, const uint64_t value) {
  for (size_t index = 0; index < 8; ++index) data[index] = static_cast<uint8_t>(value >> (index * 8U));
}

const char* pathFor(const CacheSlot slot) { return slot == CacheSlot::A ? SNAPSHOT_SLOT_A_PATH : SNAPSHOT_SLOT_B_PATH; }

void encodeHeader(uint8_t (&header)[CACHE_HEADER_BYTES], const uint64_t generation, const char* payload,
                  const size_t payloadLength) {
  std::memset(header, 0, sizeof(header));
  std::memcpy(header, CACHE_MAGIC, sizeof(CACHE_MAGIC) - 1);
  writeLe16(header + 8, CACHE_FORMAT_VERSION);
  writeLe16(header + 10, CACHE_FLAGS);
  writeLe64(header + 12, generation);
  writeLe32(header + 20, static_cast<uint32_t>(payloadLength));
  uint32_t crc = pocketCacheCrc32(header + 8, 16);
  crc = pocketCacheCrc32(payload, payloadLength, crc);
  writeLe32(header + 24, crc);
}

CacheResult decodeSlot(const uint8_t* header, const size_t headerBytes, const char* payload, const size_t payloadBytes,
                       const size_t fileSize, uint64_t& generation) {
  generation = 0;
  if (header == nullptr || headerBytes != CACHE_HEADER_BYTES ||
      std::memcmp(header, CACHE_MAGIC, sizeof(CACHE_MAGIC) - 1) != 0) {
    return CacheResult::InvalidHeader;
  }
  if (readLe16(header + 8) != CACHE_FORMAT_VERSION) return CacheResult::UnsupportedCacheFormat;
  if (readLe16(header + 10) != CACHE_FLAGS) return CacheResult::InvalidHeader;
  const uint64_t candidate = readLe64(header + 12);
  const uint32_t payloadLength = readLe32(header + 20);
  if (candidate == 0 || payloadLength == 0 || payloadLength > MAX_SNAPSHOT_JSON_BYTES ||
      fileSize != CACHE_HEADER_BYTES + payloadLength || payload == nullptr || payloadBytes != payloadLength) {
    return CacheResult::InvalidLength;
  }
  uint32_t crc = pocketCacheCrc32(header + 8, 16);
  crc = pocketCacheCrc32(payload, payloadLength, crc);
  if (crc != readLe32(header + 24)) return CacheResult::ChecksumMismatch;
  if (validatePocketSnapshot(payload, payloadLength) != SnapshotParseResult::Success) {
    return CacheResult::InvalidJsonBundle;
  }
  generation = candidate;
  return CacheResult::Success;
}

CacheOutcome failureFor(const CacheOutcome& a, const CacheOutcome& b) {
  if (a.result == CacheResult::FileNotFound && b.result == CacheResult::FileNotFound) {
    return {CacheResult::FileNotFound, CacheSlot::None, 0};
  }
  return {CacheResult::NoValidSlot, CacheSlot::None, 0};
}

}  // namespace

CacheOutcome PocketSnapshotCache::inspect(const CacheSlot slot, char* jsonBuffer, const size_t jsonCapacity,
                                          size_t* payloadLength) {
  if (payloadLength != nullptr) *payloadLength = 0;
  if (jsonBuffer == nullptr || jsonCapacity != MAX_SNAPSHOT_JSON_BYTES + 1) {
    return {CacheResult::ReadFailure, slot, 0};
  }
  uint8_t header[CACHE_HEADER_BYTES]{};
  size_t headerRead = 0;
  size_t payloadRead = 0;
  size_t fileSize = 0;
  const CacheResult read = storage.read(pathFor(slot), header, sizeof(header), headerRead, jsonBuffer, jsonCapacity - 1,
                                        payloadRead, fileSize);
  if (read != CacheResult::Success) return {read, slot, 0};
  if (payloadRead <= MAX_SNAPSHOT_JSON_BYTES) jsonBuffer[payloadRead] = '\0';
  uint64_t generation = 0;
  const CacheResult result = decodeSlot(header, headerRead, jsonBuffer, payloadRead, fileSize, generation);
  if (result == CacheResult::Success && payloadLength != nullptr) *payloadLength = payloadRead;
  return {result, slot, result == CacheResult::Success ? generation : 0};
}

CacheOutcome PocketSnapshotCache::loadBest(PocketSnapshot& destination) {
  if (!storage.available()) return {CacheResult::SdUnavailable, CacheSlot::None, 0};
  std::unique_ptr<char[]> jsonBuffer(new (std::nothrow) char[MAX_SNAPSHOT_JSON_BYTES + 1]);
  if (!jsonBuffer) return {CacheResult::ReadFailure, CacheSlot::None, 0};
  const CacheOutcome a = inspect(CacheSlot::A, jsonBuffer.get(), MAX_SNAPSHOT_JSON_BYTES + 1);
  const CacheOutcome b = inspect(CacheSlot::B, jsonBuffer.get(), MAX_SNAPSHOT_JSON_BYTES + 1);
  if (a.result != CacheResult::Success && b.result != CacheResult::Success) return failureFor(a, b);
  const CacheOutcome best =
      b.result == CacheResult::Success && (a.result != CacheResult::Success || b.generation > a.generation) ? b : a;
  size_t payloadLength = 0;
  const CacheOutcome verified = inspect(best.slot, jsonBuffer.get(), MAX_SNAPSHOT_JSON_BYTES + 1, &payloadLength);
  if (verified.result != CacheResult::Success || verified.generation != best.generation) {
    return {CacheResult::ReadFailure, best.slot, 0};
  }
  if (parsePocketSnapshot(jsonBuffer.get(), payloadLength, destination) != SnapshotParseResult::Success) {
    return {CacheResult::InvalidJsonBundle, best.slot, 0};
  }
  return best;
}

CacheOutcome PocketSnapshotCache::store(const char* json, const size_t jsonLength) {
  if (validatePocketSnapshot(json, jsonLength) != SnapshotParseResult::Success) {
    return {CacheResult::InvalidJsonBundle, CacheSlot::None, 0};
  }
  if (!storage.available()) return {CacheResult::SdUnavailable, CacheSlot::None, 0};
  std::unique_ptr<char[]> jsonBuffer(new (std::nothrow) char[MAX_SNAPSHOT_JSON_BYTES + 1]);
  if (!jsonBuffer) return {CacheResult::ReadFailure, CacheSlot::None, 0};
  const CacheOutcome a = inspect(CacheSlot::A, jsonBuffer.get(), MAX_SNAPSHOT_JSON_BYTES + 1);
  const CacheOutcome b = inspect(CacheSlot::B, jsonBuffer.get(), MAX_SNAPSHOT_JSON_BYTES + 1);
  CacheOutcome newest{CacheResult::NoValidSlot, CacheSlot::None, 0};
  if (a.result == CacheResult::Success) newest = a;
  if (b.result == CacheResult::Success && (newest.slot == CacheSlot::None || b.generation > newest.generation)) {
    newest = b;
  }
  if (newest.generation == std::numeric_limits<uint64_t>::max()) {
    return {CacheResult::GenerationExhausted, newest.slot, newest.generation};
  }
  const CacheSlot target = newest.slot == CacheSlot::A ? CacheSlot::B : CacheSlot::A;
  const uint64_t generation = newest.slot == CacheSlot::None ? 1 : newest.generation + 1;
  if (!storage.ensureDirectory(CACHE_DIRECTORY)) return {CacheResult::WriteFailure, target, generation};
  uint8_t header[CACHE_HEADER_BYTES];
  encodeHeader(header, generation, json, jsonLength);
  if (storage.write(pathFor(target), header, sizeof(header), json, jsonLength) != CacheResult::Success) {
    return {CacheResult::WriteFailure, target, generation};
  }
  const CacheOutcome verified = inspect(target, jsonBuffer.get(), MAX_SNAPSHOT_JSON_BYTES + 1);
  return verified.result == CacheResult::Success && verified.generation == generation
             ? CacheOutcome{CacheResult::Success, target, generation}
             : CacheOutcome{CacheResult::VerificationFailure, target, generation};
}

}  // namespace pocket
