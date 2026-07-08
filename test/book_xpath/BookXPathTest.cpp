#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "BookXPath.h"

// ============================================================================
// In-memory stored-ZIP fixture: BookXPath streams chapters through the
// engine's ZipEntryReader, which reads the entry's local file header first,
// so the buffer must be a structurally valid (uncompressed) zip member.
// ============================================================================

namespace {

using freeink::book::BookSource;
using freeink::book::ZipCatalog;
using freeink::book::ZipEntry;

class MemorySource : public BookSource {
 public:
  explicit MemorySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (offset >= bytes_.size()) return 0;
    const uint32_t n = std::min<uint64_t>(len, bytes_.size() - offset);
    memcpy(dst, bytes_.data() + offset, n);
    return static_cast<int32_t>(n);
  }
  uint64_t size() const override { return bytes_.size(); }

 private:
  std::vector<uint8_t> bytes_;
};

void putU16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(x & 0xFF);
  v.push_back(x >> 8);
}
void putU32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}

// One stored (method 0) zip member at offset 0, plus the matching ZipEntry.
struct Fixture {
  MemorySource source;
  ZipEntry entry;
  ZipCatalog zip;  // unused by BookXPath but part of its signature

  static Fixture fromXhtml(const std::string& xhtml) {
    std::vector<uint8_t> buf;
    const char* name = "ch.xhtml";
    putU32(buf, 0x04034b50);  // local file header signature
    putU16(buf, 20);          // version needed
    putU16(buf, 0);           // flags
    putU16(buf, 0);           // method: stored
    putU16(buf, 0);           // mod time
    putU16(buf, 0);           // mod date
    putU32(buf, 0);           // crc-32 (not validated on stored reads)
    putU32(buf, static_cast<uint32_t>(xhtml.size()));  // compressed size
    putU32(buf, static_cast<uint32_t>(xhtml.size()));  // uncompressed size
    putU16(buf, static_cast<uint16_t>(strlen(name)));  // name length
    putU16(buf, 0);                                    // extra length
    buf.insert(buf.end(), name, name + strlen(name));
    buf.insert(buf.end(), xhtml.begin(), xhtml.end());

    Fixture f{MemorySource(std::move(buf)), ZipEntry{}, ZipCatalog{}};
    f.entry.name = "ch.xhtml";
    f.entry.method = 0;
    f.entry.compressedSize = static_cast<uint32_t>(xhtml.size());
    f.entry.uncompressedSize = static_cast<uint32_t>(xhtml.size());
    f.entry.localHeaderOffset = 0;
    return f;
  }
};

// Character accounting reference for kChapter (mirrors ChapterLayout):
//   p1 "Hello world"       chars  0..10
//   p2 "Second para here"  chars 11..26
//   p3 "Third"             chars 27..31
const char kChapter[] =
    "<?xml version=\"1.0\"?>"
    "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
    "<head><title>Ignored Title</title><style>p { color: red; }</style></head>"
    "<body>"
    "<div><p>Hello   world</p><p>Second <i>para</i> here</p></div>"
    "<p>Third</p>"
    "</body></html>";

}  // namespace

TEST(BookXPath, SpineIndexFromXpath) {
  EXPECT_EQ(BookXPath::spineIndexForXpath("/body/DocFragment[8]/body/p[4]/text().96"), 7);
  EXPECT_EQ(BookXPath::spineIndexForXpath("/body/DocFragment[1]"), 0);
  EXPECT_EQ(BookXPath::spineIndexForXpath("/body/nope"), -1);
  EXPECT_EQ(BookXPath::spineIndexForXpath(""), -1);
}

TEST(BookXPath, CharStartToXpathRealAncestry) {
  auto f = Fixture::fromXhtml(kChapter);

  // Char 0: first char of p1 inside the div.
  EXPECT_EQ(BookXPath::xpathForCharStart(f.source, f.zip, f.entry, 2, 0),
            "/body/DocFragment[3]/body/div[1]/p[1]/text().0");

  // Char 13 = "cond..." — 3rd char (offset 2) of p2. Whitespace in p1
  // ("Hello   world") must have collapsed to a single space for this to hold.
  EXPECT_EQ(BookXPath::xpathForCharStart(f.source, f.zip, f.entry, 2, 13),
            "/body/DocFragment[3]/body/div[1]/p[2]/text().2");

  // Char 28 = 2nd char of p3 — body's FIRST p child (div doesn't count:
  // sibling indices are per-tag).
  EXPECT_EQ(BookXPath::xpathForCharStart(f.source, f.zip, f.entry, 2, 28),
            "/body/DocFragment[3]/body/p[1]/text().1");

  // Past the end of the chapter: chapter-level fallback.
  EXPECT_EQ(BookXPath::xpathForCharStart(f.source, f.zip, f.entry, 2, 100000), "/body/DocFragment[3]");
}

TEST(BookXPath, XpathToCharStartRoundTrip) {
  auto f = Fixture::fromXhtml(kChapter);

  uint32_t ch = 0;
  ASSERT_TRUE(BookXPath::charStartForXpath(f.source, f.zip, f.entry,
                                           "/body/DocFragment[3]/body/div[1]/p[2]/text().2", &ch));
  EXPECT_EQ(ch, 13u);

  ASSERT_TRUE(
      BookXPath::charStartForXpath(f.source, f.zip, f.entry, "/body/DocFragment[3]/body/p[1]/text().1", &ch));
  EXPECT_EQ(ch, 28u);

  // Elements without an explicit index default to [1].
  ASSERT_TRUE(
      BookXPath::charStartForXpath(f.source, f.zip, f.entry, "/body/DocFragment[3]/body/div/p[1]/text().5", &ch));
  EXPECT_EQ(ch, 5u);

  // Chapter-level xpath resolves to the chapter start.
  ASSERT_TRUE(BookXPath::charStartForXpath(f.source, f.zip, f.entry, "/body/DocFragment[3]", &ch));
  EXPECT_EQ(ch, 0u);

  // Ancestry that does not exist in the chapter.
  EXPECT_FALSE(BookXPath::charStartForXpath(f.source, f.zip, f.entry,
                                            "/body/DocFragment[3]/body/section[2]/p[9]/text().0", &ch));
}

TEST(BookXPath, InlineElementTargetsResolve) {
  auto f = Fixture::fromXhtml(kChapter);
  // KOReader may point inside an inline element: <i>para</i> starts at
  // char 18 ("Second " = 7 chars into p2, which starts at 11).
  uint32_t ch = 0;
  ASSERT_TRUE(BookXPath::charStartForXpath(f.source, f.zip, f.entry,
                                           "/body/DocFragment[3]/body/div[1]/p[2]/i[1]/text().0", &ch));
  EXPECT_EQ(ch, 18u);
}

TEST(BookXPath, EntitiesAndBrAccounting) {
  // "A&amp;B" is 3 chars; <br/> contributes one '\n' char (layout parity).
  auto f = Fixture::fromXhtml(
      "<html><body><p>A&amp;B<br/>C</p></body></html>");
  EXPECT_EQ(BookXPath::xpathForCharStart(f.source, f.zip, f.entry, 0, 4),
            "/body/DocFragment[1]/body/p[1]/text().4");  // 'C' after A,&,B,\n
  uint32_t ch = 0;
  ASSERT_TRUE(BookXPath::charStartForXpath(f.source, f.zip, f.entry, "/body/DocFragment[1]/body/p[1]/text().4", &ch));
  EXPECT_EQ(ch, 4u);
}
