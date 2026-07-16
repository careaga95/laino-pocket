#include "PocketBundleCache.h"

#include <cstring>
#include <limits>

#include "PocketCardParser.h"

namespace pocket {
namespace {

constexpr char CACHE_MAGIC[] = "LPCACHE1";

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t readLe64(const uint8_t* data) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (i * 8U);
  return value;
}

void writeLe16(uint8_t* data, const uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32(uint8_t* data, const uint32_t value) {
  for (size_t i = 0; i < 4; ++i) data[i] = static_cast<uint8_t>(value >> (i * 8U));
}

void writeLe64(uint8_t* data, const uint64_t value) {
  for (size_t i = 0; i < 8; ++i) data[i] = static_cast<uint8_t>(value >> (i * 8U));
}

const char* slotPath(const CacheSlot slot) { return slot == CacheSlot::A ? CACHE_SLOT_A_PATH : CACHE_SLOT_B_PATH; }

CacheOutcome chooseFailure(const CacheOutcome& a, const CacheOutcome& b) {
  if (a.result == CacheResult::FileNotFound && b.result == CacheResult::FileNotFound) {
    return {CacheResult::FileNotFound, CacheSlot::None, 0};
  }
  if (a.result != CacheResult::FileNotFound && b.result == CacheResult::FileNotFound) return a;
  if (b.result != CacheResult::FileNotFound && a.result == CacheResult::FileNotFound) return b;
  return {CacheResult::NoValidSlot, CacheSlot::None, 0};
}

}  // namespace

uint32_t pocketCacheCrc32(const void* data, const size_t length, const uint32_t previous) {
  uint32_t crc = ~previous;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

void encodePocketCacheHeader(uint8_t (&header)[CACHE_HEADER_BYTES], const uint64_t generation, const char* payload,
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

CacheResult decodePocketCacheSlot(const uint8_t* header, const size_t headerBytes, const char* payload,
                                  const size_t payloadBytes, const size_t fileSize, uint64_t& generation) {
  generation = 0;
  if (header == nullptr || headerBytes != CACHE_HEADER_BYTES) return CacheResult::InvalidHeader;
  if (std::memcmp(header, CACHE_MAGIC, sizeof(CACHE_MAGIC) - 1) != 0) return CacheResult::InvalidHeader;
  if (readLe16(header + 8) != CACHE_FORMAT_VERSION) return CacheResult::UnsupportedCacheFormat;
  if (readLe16(header + 10) != CACHE_FLAGS) return CacheResult::InvalidHeader;
  const uint64_t candidateGeneration = readLe64(header + 12);
  if (candidateGeneration == 0) return CacheResult::InvalidHeader;
  const uint32_t payloadLength = readLe32(header + 20);
  if (payloadLength == 0 || payloadLength > MAX_JSON_DOCUMENT_BYTES) return CacheResult::InvalidLength;
  if (fileSize != CACHE_HEADER_BYTES + payloadLength || payload == nullptr || payloadBytes != payloadLength) {
    return CacheResult::InvalidLength;
  }
  uint32_t crc = pocketCacheCrc32(header + 8, 16);
  crc = pocketCacheCrc32(payload, payloadLength, crc);
  if (crc != readLe32(header + 24)) return CacheResult::ChecksumMismatch;
  if (validateCardBundle(payload, payloadLength) != ParseResult::Success) return CacheResult::InvalidJsonBundle;
  generation = candidateGeneration;
  return CacheResult::Success;
}

CacheOutcome PocketBundleCache::inspect(const CacheSlot slot, size_t* payloadLength) {
  if (payloadLength != nullptr) *payloadLength = 0;
  uint8_t header[CACHE_HEADER_BYTES]{};
  size_t headerRead = 0;
  size_t payloadRead = 0;
  size_t fileSize = 0;
  const CacheResult readResult = storage.read(slotPath(slot), header, sizeof(header), headerRead, jsonBuffer,
                                              sizeof(jsonBuffer) - 1, payloadRead, fileSize);
  if (readResult != CacheResult::Success) return {readResult, slot, 0};
  if (payloadRead <= MAX_JSON_DOCUMENT_BYTES) jsonBuffer[payloadRead] = '\0';
  uint64_t generation = 0;
  const CacheResult result = decodePocketCacheSlot(header, headerRead, jsonBuffer, payloadRead, fileSize, generation);
  if (result == CacheResult::Success && payloadLength != nullptr) *payloadLength = payloadRead;
  return {result, slot, result == CacheResult::Success ? generation : 0};
}

CacheOutcome PocketBundleCache::loadBest(CardBundle& destination) {
  if (!storage.available()) return {CacheResult::SdUnavailable, CacheSlot::None, 0};
  const CacheOutcome a = inspect(CacheSlot::A);
  const CacheOutcome b = inspect(CacheSlot::B);
  if (a.result != CacheResult::Success && b.result != CacheResult::Success) return chooseFailure(a, b);

  const CacheOutcome best =
      b.result == CacheResult::Success && (a.result != CacheResult::Success || b.generation > a.generation) ? b : a;
  size_t payloadLength = 0;
  const CacheOutcome verified = inspect(best.slot, &payloadLength);
  if (verified.result != CacheResult::Success || verified.generation != best.generation) {
    return {CacheResult::ReadFailure, best.slot, 0};
  }
  const ParseResult parseResult = parseCardBundle(jsonBuffer, payloadLength, destination);
  if (parseResult != ParseResult::Success) return {CacheResult::InvalidJsonBundle, best.slot, 0};
  return best;
}

CacheOutcome PocketBundleCache::store(const char* json, const size_t jsonLength) {
  if (validateCardBundle(json, jsonLength) != ParseResult::Success) {
    return {CacheResult::InvalidJsonBundle, CacheSlot::None, 0};
  }
  if (!storage.available()) return {CacheResult::SdUnavailable, CacheSlot::None, 0};

  const CacheOutcome a = inspect(CacheSlot::A);
  const CacheOutcome b = inspect(CacheSlot::B);
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
  encodePocketCacheHeader(header, generation, json, jsonLength);
  const CacheResult writeResult = storage.write(slotPath(target), header, sizeof(header), json, jsonLength);
  if (writeResult != CacheResult::Success) return {CacheResult::WriteFailure, target, generation};

  const CacheOutcome verified = inspect(target);
  if (verified.result != CacheResult::Success || verified.generation != generation) {
    return {CacheResult::VerificationFailure, target, generation};
  }
  return {CacheResult::Success, target, generation};
}

const char* cacheResultName(const CacheResult result) {
  switch (result) {
    case CacheResult::Success:
      return "success";
    case CacheResult::NoValidSlot:
      return "no_valid_slot";
    case CacheResult::SdUnavailable:
      return "sd_unavailable";
    case CacheResult::FileNotFound:
      return "file_not_found";
    case CacheResult::ReadFailure:
      return "read_failure";
    case CacheResult::WriteFailure:
      return "write_failure";
    case CacheResult::InvalidHeader:
      return "invalid_header";
    case CacheResult::UnsupportedCacheFormat:
      return "unsupported_cache_format";
    case CacheResult::InvalidLength:
      return "invalid_length";
    case CacheResult::ChecksumMismatch:
      return "checksum_mismatch";
    case CacheResult::InvalidJsonBundle:
      return "invalid_json_bundle";
    case CacheResult::VerificationFailure:
      return "verification_failure";
    case CacheResult::GenerationExhausted:
      return "generation_exhausted";
  }
  return "unknown";
}

const char* cacheSlotName(const CacheSlot slot) {
  if (slot == CacheSlot::A) return "a";
  if (slot == CacheSlot::B) return "b";
  return "none";
}

}  // namespace pocket
