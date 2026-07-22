#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "PocketSnapshotCache.h"
#include "PocketSnapshotParser.h"

namespace {

constexpr char VALID[] =
    R"({"protocolVersion":2,"snapshotId":"snapshot-1","generatedAtEpoch":1774252800,"refreshAfterEpoch":1774260000,"sourceStatus":{"calendar":"fresh","tasks":"partial","commitments":"fresh"},"today":{"nextMeeting":null,"priorities":[],"counts":{"agenda":1,"tasks":1,"waiting":306,"owe":404}},"sections":[{"id":"agenda","label":"Agenda","total":1,"items":[{"id":"event-opaque","title":"Product review","subtitle":"10:00 AM–10:30 AM","detail":["Room 2","Alice","Organizer: Bob"]}]},{"id":"tasks","label":"Tasks","total":1,"items":[{"id":"task-opaque","title":"Send brief","subtitle":"Due Jul 20 · Work","detail":[]}]},{"id":"waiting","label":"Waiting On","total":306,"items":[]},{"id":"owe","label":"You Owe","total":404,"items":[]}]})";

class MemoryStorage final : public pocket::PocketCacheStorage {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  bool available() const override { return true; }
  bool ensureDirectory(const char*) override { return true; }
  pocket::CacheResult read(const char* path, uint8_t* header, const size_t headerCapacity, size_t& headerRead,
                           char* payload, const size_t payloadCapacity, size_t& payloadRead,
                           size_t& fileSize) override {
    const auto found = files.find(path);
    if (found == files.end()) return pocket::CacheResult::FileNotFound;
    fileSize = found->second.size();
    headerRead = std::min(headerCapacity, fileSize);
    std::memcpy(header, found->second.data(), headerRead);
    payloadRead = std::min(payloadCapacity, fileSize - headerRead);
    std::memcpy(payload, found->second.data() + headerRead, payloadRead);
    return pocket::CacheResult::Success;
  }
  pocket::CacheResult write(const char* path, const uint8_t* header, const size_t headerLength, const char* payload,
                            const size_t payloadLength) override {
    auto& file = files[path];
    file.assign(header, header + headerLength);
    file.insert(file.end(), payload, payload + payloadLength);
    return pocket::CacheResult::Success;
  }
};

TEST(PocketSnapshotParser, ParsesBoundedHierarchyAndSourceState) {
  pocket::PocketSnapshot snapshot;
  ASSERT_EQ(pocket::parsePocketSnapshot(VALID, std::strlen(VALID), snapshot), pocket::SnapshotParseResult::Success);
  EXPECT_EQ(snapshot.sectionCount, 4);
  EXPECT_EQ(snapshot.itemCount, 2);
  EXPECT_EQ(snapshot.tasksState, pocket::SnapshotSourceState::Partial);
  ASSERT_NE(snapshot.sectionAt(0), nullptr);
  const auto* item = snapshot.itemAt(*snapshot.sectionAt(0), 0);
  ASSERT_NE(item, nullptr);
  EXPECT_STREQ(item->title, "Product review");
  EXPECT_STREQ(item->detail[0], "Room 2");
  EXPECT_EQ(item->detailCount, 3);
  EXPECT_EQ(snapshot.sectionAt(2)->total, 306U);
  EXPECT_EQ(snapshot.sectionAt(3)->total, 404U);
  EXPECT_LT(sizeof(snapshot), 256U);
}

TEST(PocketSnapshotParser, RejectsUnsupportedProtocolAndLeavesDestinationUnchanged) {
  pocket::PocketSnapshot snapshot;
  ASSERT_EQ(pocket::parsePocketSnapshot(VALID, std::strlen(VALID), snapshot), pocket::SnapshotParseResult::Success);
  const auto* originalItem = snapshot.itemAt(*snapshot.sectionAt(0), 0);
  ASSERT_NE(originalItem, nullptr);
  std::string invalid = VALID;
  invalid.replace(invalid.find("\"protocolVersion\":2"), std::strlen("\"protocolVersion\":2"), "\"protocolVersion\":3");
  EXPECT_EQ(pocket::parsePocketSnapshot(invalid.data(), invalid.size(), snapshot),
            pocket::SnapshotParseResult::UnsupportedProtocolVersion);
  EXPECT_EQ(snapshot.generatedAtEpoch, 1774252800U);
  EXPECT_STREQ(snapshot.itemAt(*snapshot.sectionAt(0), 0)->title, originalItem->title);
}

TEST(PocketSnapshotParser, EmptySnapshotReleasesDynamicItems) {
  pocket::PocketSnapshot snapshot;
  ASSERT_EQ(pocket::parsePocketSnapshot(VALID, std::strlen(VALID), snapshot), pocket::SnapshotParseResult::Success);
  ASSERT_NE(snapshot.items, nullptr);
  pocket::loadEmptySnapshot(snapshot);
  EXPECT_EQ(snapshot.items, nullptr);
  EXPECT_EQ(snapshot.itemCount, 0);
  EXPECT_TRUE(snapshot.fixture);
}

TEST(PocketSnapshotParser, RejectsOversizedSection) {
  std::string items;
  for (int index = 0; index < 13; ++index) {
    if (!items.empty()) items += ',';
    items += R"({"id":"x","title":"T","subtitle":"S","detail":[]})";
  }
  const std::string json =
      R"({"protocolVersion":2,"snapshotId":"s","generatedAtEpoch":1,"refreshAfterEpoch":2,"sourceStatus":{"calendar":"fresh","tasks":"fresh","commitments":"fresh"},"sections":[{"id":"agenda","label":"Agenda","total":13,"items":[)" +
      items + "]}]}";
  pocket::PocketSnapshot snapshot;
  EXPECT_EQ(pocket::parsePocketSnapshot(json.data(), json.size(), snapshot), pocket::SnapshotParseResult::TooManyItems);
}

TEST(PocketSnapshotCache, AlternatesSlotsAndLoadsNewestVerifiedSnapshot) {
  MemoryStorage storage;
  pocket::PocketSnapshotCache cache(storage);
  const auto first = cache.store(VALID, std::strlen(VALID));
  ASSERT_EQ(first.result, pocket::CacheResult::Success);
  EXPECT_EQ(first.slot, pocket::CacheSlot::A);
  const auto second = cache.store(VALID, std::strlen(VALID));
  ASSERT_EQ(second.result, pocket::CacheResult::Success);
  EXPECT_EQ(second.slot, pocket::CacheSlot::B);
  pocket::PocketSnapshot snapshot;
  const auto loaded = cache.loadBest(snapshot);
  EXPECT_EQ(loaded.generation, 2);
  EXPECT_EQ(snapshot.itemCount, 2);
  EXPECT_LT(sizeof(cache), 64U);
}

}  // namespace
