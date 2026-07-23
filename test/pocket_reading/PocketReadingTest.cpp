#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "PocketReading.h"
#include "PocketReadingCache.h"
#include "PocketReadingParser.h"

namespace {

constexpr char VALID_ID[] = "00112233-4455-4677-8899-aabbccddeeff";
constexpr char VALID_SHA[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string itemJson(const char* id = VALID_ID, const char* title = "An article") {
  return std::string("{\"id\":\"") + id + "\",\"title\":\"" + title +
         "\",\"byline\":\"Ada\",\"queuedAtEpoch\":1776956400,\"bytes\":1234,\"sha256\":\"" + VALID_SHA +
         "\",\"format\":\"epub\"}";
}

std::string manifestJson(const std::string& items) {
  return std::string("{\"protocolVersion\":1,\"generatedAtEpoch\":1776956500,\"items\":[") + items + "]}";
}

class MemoryStorage final : public pocket::PocketCacheStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  bool online = true;

  bool available() const override { return online; }
  bool ensureDirectory(const char*) override { return online; }
  pocket::CacheResult read(const char* path, uint8_t* header, size_t headerCapacity, size_t& headerRead, char* payload,
                           size_t payloadCapacity, size_t& payloadRead, size_t& fileSize) override {
    headerRead = 0;
    payloadRead = 0;
    fileSize = 0;
    const auto found = files.find(path);
    if (found == files.end()) return pocket::CacheResult::FileNotFound;
    fileSize = found->second.size();
    headerRead = std::min(headerCapacity, fileSize);
    std::memcpy(header, found->second.data(), headerRead);
    payloadRead = std::min(payloadCapacity, fileSize - headerRead);
    std::memcpy(payload, found->second.data() + headerRead, payloadRead);
    return pocket::CacheResult::Success;
  }
  pocket::CacheResult write(const char* path, const uint8_t* header, size_t headerLength, const char* payload,
                            size_t payloadLength) override {
    auto& destination = files[path];
    destination.assign(header, header + headerLength);
    destination.insert(destination.end(), payload, payload + payloadLength);
    return pocket::CacheResult::Success;
  }
};

}  // namespace

TEST(PocketReadingParserTest, ParsesDeviceManifestAndBuildsOnlyIdBasedPaths) {
  const std::string json = manifestJson(itemJson());
  pocket::ReadingManifest manifest;
  ASSERT_EQ(pocket::parseReadingManifest(json.data(), json.size(), manifest), pocket::ReadingParseResult::Success);
  ASSERT_EQ(manifest.itemCount, 1);
  ASSERT_NE(manifest.itemAt(0), nullptr);
  EXPECT_STREQ(manifest.itemAt(0)->title, "An article");
  EXPECT_STREQ(manifest.itemAt(0)->byline, "Ada");
  EXPECT_EQ(manifest.itemAt(0)->bytes, 1234U);

  char finalPath[96];
  char partPath[112];
  EXPECT_TRUE(pocket::readingPath(*manifest.itemAt(0), finalPath, sizeof(finalPath)));
  EXPECT_TRUE(pocket::readingPartPath(*manifest.itemAt(0), partPath, sizeof(partPath)));
  EXPECT_STREQ(finalPath, "/Hari/Reading/00112233-4455-4677-8899-aabbccddeeff.epub");
  EXPECT_STREQ(partPath, "/Hari/Reading/.download-00112233-4455-4677-8899-aabbccddeeff.part");
  EXPECT_EQ(std::strstr(finalPath, manifest.itemAt(0)->title), nullptr);
}

TEST(PocketReadingParserTest, AcceptsEmptyQueueAndAbsentByline) {
  std::string item = itemJson();
  const std::string byline = ",\"byline\":\"Ada\"";
  item.erase(item.find(byline), byline.size());
  pocket::ReadingManifest manifest;
  const std::string withoutByline = manifestJson(item);
  ASSERT_EQ(pocket::parseReadingManifest(withoutByline.data(), withoutByline.size(), manifest),
            pocket::ReadingParseResult::Success);
  EXPECT_STREQ(manifest.itemAt(0)->byline, "");

  const std::string empty = manifestJson("");
  EXPECT_EQ(pocket::parseReadingManifest(empty.data(), empty.size(), manifest), pocket::ReadingParseResult::Success);
  EXPECT_EQ(manifest.itemCount, 0);
}

TEST(PocketReadingParserTest, RejectsNonV4UuidDuplicateUnsafeDigestAndInvalidLength) {
  pocket::ReadingManifest manifest;
  std::string json = manifestJson(itemJson("00112233-4455-6677-8899-aabbccddeeff"));
  EXPECT_EQ(pocket::parseReadingManifest(json.data(), json.size(), manifest), pocket::ReadingParseResult::InvalidValue);

  json = manifestJson(itemJson() + "," + itemJson());
  EXPECT_EQ(pocket::parseReadingManifest(json.data(), json.size(), manifest), pocket::ReadingParseResult::DuplicateItem);

  json = manifestJson(itemJson());
  json.replace(json.find(VALID_SHA), 1, "A");
  EXPECT_EQ(pocket::parseReadingManifest(json.data(), json.size(), manifest), pocket::ReadingParseResult::InvalidValue);

  json = manifestJson(itemJson());
  json.replace(json.find("\"bytes\":1234"), std::strlen("\"bytes\":1234"), "\"bytes\":0");
  EXPECT_EQ(pocket::parseReadingManifest(json.data(), json.size(), manifest), pocket::ReadingParseResult::InvalidValue);
}

TEST(PocketReadingParserTest, BoundsItemsDocumentAndTextWithoutChangingPriorDestination) {
  std::string items;
  for (size_t index = 0; index < pocket::MAX_READING_ITEMS + 1; ++index) {
    if (!items.empty()) items += ",";
    char id[sizeof(VALID_ID)];
    std::snprintf(id, sizeof(id), "00112233-4455-4677-8899-aabbccddee%02x", static_cast<unsigned>(index));
    items += itemJson(id);
  }
  pocket::ReadingManifest manifest;
  const std::string tooMany = manifestJson(items);
  EXPECT_EQ(pocket::parseReadingManifest(tooMany.data(), tooMany.size(), manifest),
            pocket::ReadingParseResult::TooManyItems);

  const std::string valid = manifestJson(itemJson());
  ASSERT_EQ(pocket::parseReadingManifest(valid.data(), valid.size(), manifest), pocket::ReadingParseResult::Success);
  const std::string originalTitle = manifest.itemAt(0)->title;
  const std::string tooLong = manifestJson(itemJson(VALID_ID, std::string(449, 'x').c_str()));
  EXPECT_EQ(pocket::parseReadingManifest(tooLong.data(), tooLong.size(), manifest),
            pocket::ReadingParseResult::TextTooLong);
  EXPECT_EQ(manifest.itemCount, 1);
  EXPECT_EQ(manifest.itemAt(0)->title, originalTitle);

  const std::string oversized(pocket::MAX_READING_MANIFEST_JSON_BYTES + 1, ' ');
  EXPECT_EQ(pocket::parseReadingManifest(oversized.data(), oversized.size(), manifest),
            pocket::ReadingParseResult::DocumentTooLarge);
}

TEST(PocketReadingCacheTest, AlternatesVerifiedSlotsAndFallsBackToLastValidManifest) {
  MemoryStorage storage;
  pocket::PocketReadingCache cache(storage);
  const std::string first = manifestJson(itemJson());
  const auto storedFirst = cache.store(first.data(), first.size());
  ASSERT_EQ(storedFirst.result, pocket::CacheResult::Success);
  EXPECT_EQ(storedFirst.slot, pocket::CacheSlot::A);

  const std::string second = manifestJson("");
  const auto storedSecond = cache.store(second.data(), second.size());
  ASSERT_EQ(storedSecond.result, pocket::CacheResult::Success);
  EXPECT_EQ(storedSecond.slot, pocket::CacheSlot::B);

  pocket::ReadingManifest loaded;
  ASSERT_EQ(cache.loadBest(loaded).result, pocket::CacheResult::Success);
  EXPECT_EQ(loaded.itemCount, 0);

  storage.files[pocket::READING_SLOT_B_PATH].back() ^= 1;
  const auto fallback = cache.loadBest(loaded);
  EXPECT_EQ(fallback.result, pocket::CacheResult::Success);
  EXPECT_EQ(fallback.slot, pocket::CacheSlot::A);
  EXPECT_EQ(loaded.itemCount, 1);
}
