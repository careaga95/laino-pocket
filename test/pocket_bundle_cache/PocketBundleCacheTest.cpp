#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "PocketBundleCache.h"
#include "PocketBundleRuntime.h"
#include "PocketCardFixture.h"
#include "PocketCardSelection.h"

namespace {

using pocket::CacheResult;
using pocket::CacheSlot;

constexpr char VALID_JSON[] =
    R"({"protocolVersion":1,"cards":[{"label":"L","title":"T","subtitle":"S","lines":["- X"]}]})";
constexpr char SECOND_JSON[] =
    R"({"protocolVersion":1,"cards":[{"label":"Second","title":"T","subtitle":"S","lines":[]}]})";
constexpr char THIRD_JSON[] =
    R"({"protocolVersion":1,"cards":[{"label":"Third","title":"T","subtitle":"S","lines":[]}]})";
constexpr char INVALID_SCHEMA_JSON[] = R"({"protocolVersion":1,"cards":[]})";

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t readLe64(const uint8_t* data) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (i * 8U);
  return value;
}

std::vector<uint8_t> makeSlot(const std::string& json, const uint64_t generation) {
  uint8_t header[pocket::CACHE_HEADER_BYTES];
  pocket::encodePocketCacheHeader(header, generation, json.data(), json.size());
  std::vector<uint8_t> file(header, header + sizeof(header));
  file.insert(file.end(), json.begin(), json.end());
  return file;
}

CacheResult decode(const std::vector<uint8_t>& file, uint64_t& generation) {
  const size_t headerBytes = std::min(file.size(), pocket::CACHE_HEADER_BYTES);
  const char* payload = file.size() > pocket::CACHE_HEADER_BYTES
                            ? reinterpret_cast<const char*>(file.data() + pocket::CACHE_HEADER_BYTES)
                            : nullptr;
  const size_t payloadBytes = file.size() > pocket::CACHE_HEADER_BYTES ? file.size() - pocket::CACHE_HEADER_BYTES : 0;
  return pocket::decodePocketCacheSlot(file.data(), headerBytes, payload, payloadBytes, file.size(), generation);
}

std::string maximumJson() {
  std::string json = VALID_JSON;
  json.pop_back();
  const std::string framing = R"(,"future":""})";
  const size_t padding = pocket::MAX_JSON_DOCUMENT_BYTES - json.size() - framing.size();
  return json + R"(,"future":")" + std::string(padding, 'x') + R"("})";
}

class MemoryStorage final : public pocket::PocketCacheStorage {
 public:
  enum class WriteMode { Success, FailNoChange, PartialHeader, PartialPayload, FailAfterComplete };

  bool isAvailable = true;
  bool directoryResult = true;
  WriteMode writeMode = WriteMode::Success;
  std::map<std::string, std::vector<uint8_t>> files;
  std::string readFailurePath;
  bool corruptVerificationRead = false;
  size_t writeCalls = 0;

  bool available() const override { return isAvailable; }
  bool ensureDirectory(const char*) override { return directoryResult; }

  CacheResult read(const char* path, uint8_t* header, const size_t headerCapacity, size_t& headerRead, char* payload,
                   const size_t payloadCapacity, size_t& payloadRead, size_t& fileSize) override {
    headerRead = 0;
    payloadRead = 0;
    fileSize = 0;
    if (!isAvailable) return CacheResult::SdUnavailable;
    if (readFailurePath == path) return CacheResult::ReadFailure;
    const auto found = files.find(path);
    if (found == files.end()) return CacheResult::FileNotFound;
    std::vector<uint8_t> copy = found->second;
    if (corruptVerificationRead) {
      corruptVerificationRead = false;
      if (copy.size() > pocket::CACHE_HEADER_BYTES) copy.back() ^= 0x01U;
    }
    fileSize = copy.size();
    headerRead = std::min(headerCapacity, copy.size());
    std::memcpy(header, copy.data(), headerRead);
    const size_t remaining = copy.size() - headerRead;
    payloadRead = std::min(payloadCapacity, remaining);
    if (payloadRead > 0) std::memcpy(payload, copy.data() + headerRead, payloadRead);
    return CacheResult::Success;
  }

  CacheResult write(const char* path, const uint8_t* header, const size_t headerLength, const char* payload,
                    const size_t payloadLength) override {
    ++writeCalls;
    if (writeMode == WriteMode::FailNoChange) return CacheResult::WriteFailure;
    std::vector<uint8_t> output;
    if (writeMode == WriteMode::PartialHeader) {
      output.assign(header, header + headerLength / 2);
    } else {
      output.assign(header, header + headerLength);
      const size_t bytes = writeMode == WriteMode::PartialPayload ? payloadLength / 2 : payloadLength;
      output.insert(output.end(), payload, payload + bytes);
    }
    files[path] = output;
    if (writeMode == WriteMode::FailAfterComplete || writeMode == WriteMode::PartialHeader ||
        writeMode == WriteMode::PartialPayload) {
      return CacheResult::WriteFailure;
    }
    if (corruptVerificationRead) return CacheResult::Success;
    return CacheResult::Success;
  }
};

void expectLoadsLabel(MemoryStorage& storage, const char* expected, const CacheSlot slot, const uint64_t generation) {
  pocket::PocketBundleCache cache(storage);
  pocket::CardBundle bundle;
  const auto result = cache.loadBest(bundle);
  ASSERT_EQ(result.result, CacheResult::Success);
  EXPECT_EQ(result.slot, slot);
  EXPECT_EQ(result.generation, generation);
  EXPECT_STREQ(bundle.cards[0].label, expected);
}

}  // namespace

TEST(PocketCacheFormatTest, HeaderIsExactly28Bytes) { EXPECT_EQ(pocket::CACHE_HEADER_BYTES, 28U); }

TEST(PocketCacheFormatTest, HeaderUsesKnownLittleEndianEncoding) {
  uint8_t header[pocket::CACHE_HEADER_BYTES];
  pocket::encodePocketCacheHeader(header, 0x0102030405060708ULL, VALID_JSON, sizeof(VALID_JSON) - 1);
  EXPECT_EQ(std::memcmp(header, "LPCACHE1", 8), 0);
  EXPECT_EQ(header[8], 1U);
  EXPECT_EQ(header[9], 0U);
  EXPECT_EQ(readLe64(header + 12), 0x0102030405060708ULL);
  EXPECT_EQ(readLe32(header + 20), sizeof(VALID_JSON) - 1);
}

TEST(PocketCacheFormatTest, Crc32MatchesStandardVector) {
  EXPECT_EQ(pocket::pocketCacheCrc32("123456789", 9), 0xCBF43926U);
}

TEST(PocketCacheFormatTest, ExactFileSizeIsAccepted) {
  uint64_t generation = 0;
  EXPECT_EQ(decode(makeSlot(VALID_JSON, 7), generation), CacheResult::Success);
  EXPECT_EQ(generation, 7U);
}

TEST(PocketCacheFormatTest, WrongMagicIsRejected) {
  auto file = makeSlot(VALID_JSON, 1);
  file[0] ^= 1U;
  uint64_t generation = 0;
  EXPECT_EQ(decode(file, generation), CacheResult::InvalidHeader);
}

TEST(PocketCacheFormatTest, UnsupportedFormatIsRejected) {
  auto file = makeSlot(VALID_JSON, 1);
  file[8] = 2;
  uint64_t generation = 0;
  EXPECT_EQ(decode(file, generation), CacheResult::UnsupportedCacheFormat);
}

TEST(PocketCacheFormatTest, NonzeroFlagsAndZeroGenerationAreRejected) {
  uint64_t generation = 0;
  auto flags = makeSlot(VALID_JSON, 1);
  flags[10] = 1;
  EXPECT_EQ(decode(flags, generation), CacheResult::InvalidHeader);
  auto zero = makeSlot(VALID_JSON, 1);
  std::fill(zero.begin() + 12, zero.begin() + 20, 0);
  EXPECT_EQ(decode(zero, generation), CacheResult::InvalidHeader);
}

TEST(PocketCacheFormatTest, ZeroAndOversizedPayloadLengthsAreRejected) {
  uint64_t generation = 0;
  auto zero = makeSlot(VALID_JSON, 1);
  std::fill(zero.begin() + 20, zero.begin() + 24, 0);
  EXPECT_EQ(decode(zero, generation), CacheResult::InvalidLength);
  auto oversized = makeSlot(VALID_JSON, 1);
  const uint32_t length = pocket::MAX_JSON_DOCUMENT_BYTES + 1;
  for (size_t i = 0; i < 4; ++i) oversized[20 + i] = static_cast<uint8_t>(length >> (i * 8U));
  EXPECT_EQ(decode(oversized, generation), CacheResult::InvalidLength);
}

TEST(PocketCacheFormatTest, TruncatedHeaderAndPayloadAndTrailingBytesAreRejected) {
  uint64_t generation = 0;
  auto header = makeSlot(VALID_JSON, 1);
  header.resize(20);
  EXPECT_EQ(decode(header, generation), CacheResult::InvalidHeader);
  auto payload = makeSlot(VALID_JSON, 1);
  payload.pop_back();
  EXPECT_EQ(decode(payload, generation), CacheResult::InvalidLength);
  auto trailing = makeSlot(VALID_JSON, 1);
  trailing.push_back(0);
  EXPECT_EQ(decode(trailing, generation), CacheResult::InvalidLength);
}

TEST(PocketCacheFormatTest, CorruptedGenerationAndPayloadFailChecksum) {
  uint64_t generation = 0;
  auto generationFile = makeSlot(VALID_JSON, 1);
  generationFile[12] ^= 2U;
  EXPECT_EQ(decode(generationFile, generation), CacheResult::ChecksumMismatch);
  auto payloadFile = makeSlot(VALID_JSON, 1);
  payloadFile.back() ^= 1U;
  EXPECT_EQ(decode(payloadFile, generation), CacheResult::ChecksumMismatch);
}

TEST(PocketCacheFormatTest, CorruptedLengthFailsValidation) {
  auto file = makeSlot(VALID_JSON, 1);
  file[20] += 1;
  uint64_t generation = 0;
  EXPECT_EQ(decode(file, generation), CacheResult::InvalidLength);
}

TEST(PocketCacheLoadingTest, BothSlotsMissingReportsFileNotFoundAndPreservesDestination) {
  MemoryStorage storage;
  pocket::PocketBundleCache cache(storage);
  pocket::CardBundle bundle;
  pocket::loadFallbackCardBundle(bundle);
  const pocket::CardBundle original = bundle;
  EXPECT_EQ(cache.loadBest(bundle).result, CacheResult::FileNotFound);
  EXPECT_EQ(std::memcmp(&bundle, &original, sizeof(bundle)), 0);
}

TEST(PocketCacheLoadingTest, SdUnavailableAndReadFailureAreDistinguished) {
  MemoryStorage unavailable;
  unavailable.isAvailable = false;
  pocket::PocketBundleCache unavailableCache(unavailable);
  pocket::CardBundle bundle;
  EXPECT_EQ(unavailableCache.loadBest(bundle).result, CacheResult::SdUnavailable);

  MemoryStorage failed;
  failed.readFailurePath = pocket::CACHE_SLOT_A_PATH;
  pocket::PocketBundleCache failedCache(failed);
  EXPECT_EQ(failedCache.loadBest(bundle).result, CacheResult::ReadFailure);
}

TEST(PocketCacheLoadingTest, OnlySlotAOrOnlySlotBValidLoads) {
  MemoryStorage a;
  a.files[pocket::CACHE_SLOT_A_PATH] = makeSlot(VALID_JSON, 1);
  expectLoadsLabel(a, "L", CacheSlot::A, 1);
  MemoryStorage b;
  b.files[pocket::CACHE_SLOT_B_PATH] = makeSlot(SECOND_JSON, 4);
  expectLoadsLabel(b, "Second", CacheSlot::B, 4);
}

TEST(PocketCacheLoadingTest, GreaterGenerationWinsInEitherSlot) {
  MemoryStorage aNewer;
  aNewer.files[pocket::CACHE_SLOT_A_PATH] = makeSlot(SECOND_JSON, 8);
  aNewer.files[pocket::CACHE_SLOT_B_PATH] = makeSlot(VALID_JSON, 7);
  expectLoadsLabel(aNewer, "Second", CacheSlot::A, 8);
  MemoryStorage bNewer;
  bNewer.files[pocket::CACHE_SLOT_A_PATH] = makeSlot(VALID_JSON, 7);
  bNewer.files[pocket::CACHE_SLOT_B_PATH] = makeSlot(SECOND_JSON, 8);
  expectLoadsLabel(bNewer, "Second", CacheSlot::B, 8);
}

TEST(PocketCacheLoadingTest, CorruptOrSchemaInvalidNewerSlotFallsBackToOlder) {
  MemoryStorage truncated;
  truncated.files[pocket::CACHE_SLOT_A_PATH] = makeSlot(VALID_JSON, 1);
  truncated.files[pocket::CACHE_SLOT_B_PATH] = makeSlot(SECOND_JSON, 2);
  truncated.files[pocket::CACHE_SLOT_B_PATH].resize(10);
  expectLoadsLabel(truncated, "L", CacheSlot::A, 1);
  MemoryStorage crc;
  crc.files[pocket::CACHE_SLOT_A_PATH] = makeSlot(VALID_JSON, 1);
  crc.files[pocket::CACHE_SLOT_B_PATH] = makeSlot(SECOND_JSON, 2);
  crc.files[pocket::CACHE_SLOT_B_PATH].back() ^= 1U;
  expectLoadsLabel(crc, "L", CacheSlot::A, 1);
  MemoryStorage schema;
  schema.files[pocket::CACHE_SLOT_A_PATH] = makeSlot(VALID_JSON, 1);
  schema.files[pocket::CACHE_SLOT_B_PATH] = makeSlot(INVALID_SCHEMA_JSON, 2);
  expectLoadsLabel(schema, "L", CacheSlot::A, 1);
}

TEST(PocketCacheLoadingTest, BothInvalidLeaveDestinationUnchanged) {
  MemoryStorage storage;
  storage.files[pocket::CACHE_SLOT_A_PATH] = {1, 2, 3};
  storage.files[pocket::CACHE_SLOT_B_PATH] = makeSlot(INVALID_SCHEMA_JSON, 2);
  pocket::PocketBundleCache cache(storage);
  pocket::CardBundle bundle;
  pocket::loadFallbackCardBundle(bundle);
  const pocket::CardBundle original = bundle;
  EXPECT_EQ(cache.loadBest(bundle).result, CacheResult::NoValidSlot);
  EXPECT_EQ(std::memcmp(&bundle, &original, sizeof(bundle)), 0);
}

TEST(PocketCacheLoadingTest, MaximumSizeDocumentLoadsAndNavigationUsesCachedCount) {
  const std::string json = maximumJson();
  ASSERT_EQ(json.size(), pocket::MAX_JSON_DOCUMENT_BYTES);
  MemoryStorage storage;
  storage.files[pocket::CACHE_SLOT_A_PATH] = makeSlot(json, 1);
  pocket::PocketBundleCache cache(storage);
  pocket::CardBundle bundle;
  ASSERT_EQ(cache.loadBest(bundle).result, CacheResult::Success);
  pocket::CardSelection selection(bundle.cardCount);
  EXPECT_EQ(selection.cardCount(), 1U);
  EXPECT_FALSE(selection.canSelectNext());
}

TEST(PocketCacheWritingTest, WritesAlternateSlotsAndIncrementGeneration) {
  MemoryStorage storage;
  pocket::PocketBundleCache cache(storage);
  auto first = cache.store(VALID_JSON, sizeof(VALID_JSON) - 1);
  EXPECT_EQ(first.slot, CacheSlot::A);
  EXPECT_EQ(first.generation, 1U);
  const auto slotA = storage.files[pocket::CACHE_SLOT_A_PATH];
  auto second = cache.store(SECOND_JSON, sizeof(SECOND_JSON) - 1);
  EXPECT_EQ(second.slot, CacheSlot::B);
  EXPECT_EQ(second.generation, 2U);
  EXPECT_EQ(storage.files[pocket::CACHE_SLOT_A_PATH], slotA);
  const auto slotB = storage.files[pocket::CACHE_SLOT_B_PATH];
  auto third = cache.store(THIRD_JSON, sizeof(THIRD_JSON) - 1);
  EXPECT_EQ(third.slot, CacheSlot::A);
  EXPECT_EQ(third.generation, 3U);
  EXPECT_EQ(storage.files[pocket::CACHE_SLOT_B_PATH], slotB);
  expectLoadsLabel(storage, "Third", CacheSlot::A, 3);
}

TEST(PocketCacheWritingTest, DirectoryFailureIsAWriteFailureWithoutModification) {
  MemoryStorage storage;
  storage.directoryResult = false;
  pocket::PocketBundleCache cache(storage);
  EXPECT_EQ(cache.store(VALID_JSON, sizeof(VALID_JSON) - 1).result, CacheResult::WriteFailure);
  EXPECT_EQ(storage.writeCalls, 0U);
  EXPECT_TRUE(storage.files.empty());
}

TEST(PocketCacheWritingTest, WriteFailuresAndPartialWritesLeavePreviousSlotLoadable) {
  for (const auto mode : {MemoryStorage::WriteMode::FailNoChange, MemoryStorage::WriteMode::PartialHeader,
                          MemoryStorage::WriteMode::PartialPayload, MemoryStorage::WriteMode::FailAfterComplete}) {
    MemoryStorage storage;
    storage.files[pocket::CACHE_SLOT_A_PATH] = makeSlot(VALID_JSON, 1);
    const auto original = storage.files[pocket::CACHE_SLOT_A_PATH];
    storage.writeMode = mode;
    pocket::PocketBundleCache cache(storage);
    EXPECT_EQ(cache.store(SECOND_JSON, sizeof(SECOND_JSON) - 1).result, CacheResult::WriteFailure);
    EXPECT_EQ(storage.files[pocket::CACHE_SLOT_A_PATH], original);
    uint64_t generation = 0;
    EXPECT_EQ(decode(storage.files[pocket::CACHE_SLOT_A_PATH], generation), CacheResult::Success);
    EXPECT_EQ(generation, 1U);
  }
}

TEST(PocketCacheWritingTest, ReadBackVerificationFailureIsReported) {
  MemoryStorage storage;
  pocket::PocketBundleCache cache(storage);
  storage.corruptVerificationRead = true;
  EXPECT_EQ(cache.store(VALID_JSON, sizeof(VALID_JSON) - 1).result, CacheResult::VerificationFailure);
}

TEST(PocketCacheWritingTest, InvalidJsonIsRejectedBeforeModification) {
  MemoryStorage storage;
  storage.files[pocket::CACHE_SLOT_A_PATH] = makeSlot(VALID_JSON, 1);
  const auto original = storage.files;
  pocket::PocketBundleCache cache(storage);
  EXPECT_EQ(cache.store(INVALID_SCHEMA_JSON, sizeof(INVALID_SCHEMA_JSON) - 1).result, CacheResult::InvalidJsonBundle);
  EXPECT_EQ(storage.writeCalls, 0U);
  EXPECT_EQ(storage.files, original);
}

TEST(PocketCacheWritingTest, GenerationOverflowIsRejected) {
  MemoryStorage storage;
  storage.files[pocket::CACHE_SLOT_A_PATH] = makeSlot(VALID_JSON, std::numeric_limits<uint64_t>::max());
  pocket::PocketBundleCache cache(storage);
  EXPECT_EQ(cache.store(SECOND_JSON, sizeof(SECOND_JSON) - 1).result, CacheResult::GenerationExhausted);
  EXPECT_EQ(storage.writeCalls, 0U);
}

TEST(PocketCacheWritingTest, SuccessfulWriteImmediatelyLoadsAndStoreDoesNotAlterCallerBundle) {
  MemoryStorage storage;
  pocket::PocketBundleCache cache(storage);
  pocket::CardBundle caller;
  pocket::loadFallbackCardBundle(caller);
  const pocket::CardBundle original = caller;
  ASSERT_EQ(cache.store(SECOND_JSON, sizeof(SECOND_JSON) - 1).result, CacheResult::Success);
  EXPECT_EQ(std::memcmp(&caller, &original, sizeof(caller)), 0);
  ASSERT_EQ(cache.loadBest(caller).result, CacheResult::Success);
  EXPECT_STREQ(caller.cards[0].label, "Second");
}

TEST(PocketCacheRuntimeTest, MissingCacheUsesFixtureAndSeeds) {
  MemoryStorage storage;
  pocket::PocketBundleCache cache(storage);
  pocket::CardBundle bundle;
  const auto outcome =
      pocket::loadPocketBundle(cache, pocket::COMPILED_CARD_JSON, pocket::COMPILED_CARD_JSON_LENGTH, bundle);
  EXPECT_EQ(outcome.source, pocket::BundleSource::CompiledFixture);
  EXPECT_EQ(outcome.seed.result, CacheResult::Success);
  EXPECT_STREQ(bundle.cards[0].label, "Laino Next");
}

TEST(PocketCacheRuntimeTest, ValidCachePreventsReseeding) {
  MemoryStorage storage;
  storage.files[pocket::CACHE_SLOT_A_PATH] = makeSlot(SECOND_JSON, 1);
  pocket::PocketBundleCache cache(storage);
  pocket::CardBundle bundle;
  const auto outcome =
      pocket::loadPocketBundle(cache, pocket::COMPILED_CARD_JSON, pocket::COMPILED_CARD_JSON_LENGTH, bundle);
  EXPECT_EQ(outcome.source, pocket::BundleSource::Cache);
  EXPECT_EQ(storage.writeCalls, 0U);
  EXPECT_STREQ(bundle.cards[0].label, "Second");
}

TEST(PocketCacheRuntimeTest, MissingSdOrFailedSeedStillUsesFixture) {
  MemoryStorage missing;
  missing.isAvailable = false;
  pocket::PocketBundleCache missingCache(missing);
  pocket::CardBundle bundle;
  auto outcome =
      pocket::loadPocketBundle(missingCache, pocket::COMPILED_CARD_JSON, pocket::COMPILED_CARD_JSON_LENGTH, bundle);
  EXPECT_EQ(outcome.source, pocket::BundleSource::CompiledFixture);
  EXPECT_EQ(outcome.seed.result, CacheResult::SdUnavailable);
  MemoryStorage failed;
  failed.writeMode = MemoryStorage::WriteMode::FailNoChange;
  pocket::PocketBundleCache failedCache(failed);
  outcome =
      pocket::loadPocketBundle(failedCache, pocket::COMPILED_CARD_JSON, pocket::COMPILED_CARD_JSON_LENGTH, bundle);
  EXPECT_EQ(outcome.source, pocket::BundleSource::CompiledFixture);
  EXPECT_EQ(outcome.seed.result, CacheResult::WriteFailure);
}

TEST(PocketCacheRuntimeTest, InvalidFixtureFallsBackToIndependentCard) {
  MemoryStorage storage;
  pocket::PocketBundleCache cache(storage);
  pocket::CardBundle bundle;
  const auto outcome = pocket::loadPocketBundle(cache, INVALID_SCHEMA_JSON, sizeof(INVALID_SCHEMA_JSON) - 1, bundle);
  EXPECT_EQ(outcome.source, pocket::BundleSource::Fallback);
  EXPECT_GE(bundle.cardCount, 1U);
  EXPECT_STREQ(bundle.cards[0].label, "Laino Next");
}
