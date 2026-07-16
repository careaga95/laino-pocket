#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "PocketCard.h"
#include "PocketCardFixture.h"
#include "PocketCardParser.h"
#include "PocketCardSelection.h"

namespace {

std::string quoted(const std::string& value) { return "\"" + value + "\""; }

std::string cardJson(const std::string& label = "Label", const std::string& title = "Title",
                     const std::string& subtitle = "Subtitle", const std::vector<std::string>& lines = {"Line"}) {
  std::string result = "{\"label\":" + quoted(label) + ",\"title\":" + quoted(title) +
                       ",\"subtitle\":" + quoted(subtitle) + ",\"lines\":[";
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) result += ',';
    result += quoted(lines[i]);
  }
  return result + "]}";
}

std::string bundleJson(const std::vector<std::string>& cards, const std::string& version = "1") {
  std::string result = "{\"protocolVersion\":" + version + ",\"cards\":[";
  for (size_t i = 0; i < cards.size(); ++i) {
    if (i != 0) result += ',';
    result += cards[i];
  }
  return result + "]}";
}

pocket::ParseResult parse(const std::string& json, pocket::CardBundle& destination) {
  return pocket::parseCardBundle(json.data(), json.size(), destination);
}

void expectFailureLeavesDestinationUnchanged(const std::string& json, const pocket::ParseResult expected) {
  pocket::CardBundle destination;
  std::memset(&destination, 0xA5, sizeof(destination));
  const pocket::CardBundle original = destination;

  EXPECT_EQ(parse(json, destination), expected);
  EXPECT_EQ(std::memcmp(&destination, &original, sizeof(destination)), 0);
}

std::string withInvalidByte() { return std::string(1, static_cast<char>(0x80)); }

}  // namespace

TEST(PocketCardParserValidTest, CompleteCompiledFixturePreservesEveryFieldAndOrder) {
  pocket::CardBundle bundle;

  ASSERT_EQ(pocket::parseCardBundle(pocket::COMPILED_CARD_JSON, pocket::COMPILED_CARD_JSON_LENGTH, bundle),
            pocket::ParseResult::Success);
  ASSERT_EQ(bundle.cardCount, 3U);

  EXPECT_STREQ(bundle.cards[0].label, "Laino Next");
  EXPECT_STREQ(bundle.cards[0].title, "Prepare renewal call");
  EXPECT_STREQ(bundle.cards[0].subtitle, "Today \xC2\xB7 11:00");
  ASSERT_EQ(bundle.cards[0].lineCount, 3U);
  EXPECT_STREQ(bundle.cards[0].lines[0], "Review premium figures");
  EXPECT_STREQ(bundle.cards[0].lines[1], "Check engineering note");
  EXPECT_STREQ(bundle.cards[0].lines[2], "Agree market strategy");

  EXPECT_STREQ(bundle.cards[1].label, "Daily Command");
  EXPECT_STREQ(bundle.cards[1].title, "Today");
  EXPECT_STREQ(bundle.cards[1].subtitle, "Wednesday \xC2\xB7 3 priorities");
  ASSERT_EQ(bundle.cards[1].lineCount, 3U);
  EXPECT_STREQ(bundle.cards[1].lines[0], "Review Pocket Phase 3");
  EXPECT_STREQ(bundle.cards[1].lines[1], "Follow up with engineering");
  EXPECT_STREQ(bundle.cards[1].lines[2], "Prepare client meeting");

  EXPECT_STREQ(bundle.cards[2].label, "Waiting On");
  EXPECT_STREQ(bundle.cards[2].title, "Waiting on");
  EXPECT_STREQ(bundle.cards[2].subtitle, "2 open items");
  ASSERT_EQ(bundle.cards[2].lineCount, 2U);
  EXPECT_STREQ(bundle.cards[2].lines[0], "Engineering note \xC2\xB7 Quantum");
  EXPECT_STREQ(bundle.cards[2].lines[1], "Updated terms \xC2\xB7 Market");
}

TEST(PocketCardParserValidTest, ExactFieldLimitsAndSixLinesAreAcceptedAndNullTerminated) {
  const std::string label(pocket::MAX_LABEL_BYTES, 'L');
  const std::string title(pocket::MAX_TITLE_BYTES, 'T');
  const std::string subtitle(pocket::MAX_SUBTITLE_BYTES, 'S');
  const std::vector<std::string> lines(pocket::MAX_LINES_PER_CARD, std::string(pocket::MAX_LINE_BYTES, 'X'));
  pocket::CardBundle bundle;

  ASSERT_EQ(parse(bundleJson({cardJson(label, title, subtitle, lines)}), bundle), pocket::ParseResult::Success);
  ASSERT_EQ(bundle.cardCount, 1U);
  EXPECT_EQ(std::strlen(bundle.cards[0].label), pocket::MAX_LABEL_BYTES);
  EXPECT_EQ(std::strlen(bundle.cards[0].title), pocket::MAX_TITLE_BYTES);
  EXPECT_EQ(std::strlen(bundle.cards[0].subtitle), pocket::MAX_SUBTITLE_BYTES);
  EXPECT_EQ(bundle.cards[0].label[pocket::MAX_LABEL_BYTES], '\0');
  EXPECT_EQ(bundle.cards[0].title[pocket::MAX_TITLE_BYTES], '\0');
  EXPECT_EQ(bundle.cards[0].subtitle[pocket::MAX_SUBTITLE_BYTES], '\0');
  for (size_t i = 0; i < pocket::MAX_LINES_PER_CARD; ++i) {
    EXPECT_EQ(std::strlen(bundle.cards[0].lines[i]), pocket::MAX_LINE_BYTES);
    EXPECT_EQ(bundle.cards[0].lines[i][pocket::MAX_LINE_BYTES], '\0');
  }
}

TEST(PocketCardParserValidTest, DocumentExactlyAtMaximumSizeIsAccepted) {
  std::string json = bundleJson({cardJson()});
  json.pop_back();
  const std::string suffix = ",\"future\":\"\"}";
  ASSERT_LT(json.size() + suffix.size(), pocket::MAX_JSON_DOCUMENT_BYTES);
  const size_t padding = pocket::MAX_JSON_DOCUMENT_BYTES - json.size() - suffix.size();
  json += ",\"future\":\"" + std::string(padding, 'p') + "\"}";
  ASSERT_EQ(json.size(), pocket::MAX_JSON_DOCUMENT_BYTES);
  pocket::CardBundle bundle;

  EXPECT_EQ(parse(json, bundle), pocket::ParseResult::Success);
  EXPECT_EQ(bundle.cardCount, 1U);
}

TEST(PocketCardParserValidTest, UnicodeEscapesAreDecodedToUtf8) {
  pocket::CardBundle bundle;
  const std::string json =
      R"({"protocolVersion":1,"cards":[{"label":"Laino","title":"Smile \uD83D\uDE03","subtitle":"Today \u00B7 11:00","lines":[""]}]})";

  ASSERT_EQ(parse(json, bundle), pocket::ParseResult::Success);
  EXPECT_STREQ(bundle.cards[0].title, "Smile \xF0\x9F\x98\x83");
  EXPECT_STREQ(bundle.cards[0].subtitle, "Today \xC2\xB7 11:00");
  EXPECT_STREQ(bundle.cards[0].lines[0], "");
}

TEST(PocketCardParserValidTest, UnknownNestedFieldsAreIgnoredSafely) {
  pocket::CardBundle bundle;
  const std::string json =
      R"({"protocolVersion":1,"future":{"nested":[1,true,null,{"x":"y"}]},"cards":[{"label":"L","unknown":[{"a":1}],"title":"T","subtitle":"","lines":[]}]})";

  EXPECT_EQ(parse(json, bundle), pocket::ParseResult::Success);
  EXPECT_EQ(bundle.cardCount, 1U);
  EXPECT_EQ(bundle.cards[0].lineCount, 0U);
}

TEST(PocketCardParserDocumentFailureTest, NullInputIsRejected) {
  pocket::CardBundle destination;
  pocket::loadFallbackCardBundle(destination);
  const pocket::CardBundle original = destination;

  EXPECT_EQ(pocket::parseCardBundle(nullptr, 10, destination), pocket::ParseResult::EmptyInput);
  EXPECT_EQ(std::memcmp(&destination, &original, sizeof(destination)), 0);
}

TEST(PocketCardParserDocumentFailureTest, EmptyInputIsRejected) {
  expectFailureLeavesDestinationUnchanged("", pocket::ParseResult::EmptyInput);
}

TEST(PocketCardParserDocumentFailureTest, OversizedDocumentIsRejectedBeforeParsing) {
  expectFailureLeavesDestinationUnchanged(std::string(pocket::MAX_JSON_DOCUMENT_BYTES + 1, ' '),
                                          pocket::ParseResult::DocumentTooLarge);
}

TEST(PocketCardParserDocumentFailureTest, MalformedJsonIsRejected) {
  expectFailureLeavesDestinationUnchanged(
      R"({"protocolVersion":1,"cards":[{"label":"L","title":"T","subtitle":"S","lines":[]})",
      pocket::ParseResult::MalformedJson);
  expectFailureLeavesDestinationUnchanged(
      bundleJson({cardJson()}).substr(0, bundleJson({cardJson()}).size() - 1) + ",}",
      pocket::ParseResult::MalformedJson);
  expectFailureLeavesDestinationUnchanged(R"({"protocolVersion":01,"cards":[]})", pocket::ParseResult::MalformedJson);
}

TEST(PocketCardParserDocumentFailureTest, TrailingNonWhitespaceContentIsRejected) {
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson()}) + "x", pocket::ParseResult::MalformedJson);
}

TEST(PocketCardParserDocumentFailureTest, NonObjectRootIsRejected) {
  expectFailureLeavesDestinationUnchanged("[]", pocket::ParseResult::WrongFieldType);
}

TEST(PocketCardParserDocumentFailureTest, MissingRootFieldsAreRejected) {
  expectFailureLeavesDestinationUnchanged("{\"cards\":[" + cardJson() + "]}",
                                          pocket::ParseResult::MissingRequiredField);
  expectFailureLeavesDestinationUnchanged(R"({"protocolVersion":1})", pocket::ParseResult::MissingRequiredField);
}

TEST(PocketCardParserDocumentFailureTest, UnsupportedProtocolVersionIsRejected) {
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson()}, "2"),
                                          pocket::ParseResult::UnsupportedProtocolVersion);
}

TEST(PocketCardParserDocumentFailureTest, ProtocolVersionWrongTypeIsRejected) {
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson()}, R"("1")"), pocket::ParseResult::WrongFieldType);
}

TEST(PocketCardParserDocumentFailureTest, CardsWrongTypeAndEmptyArrayAreRejected) {
  expectFailureLeavesDestinationUnchanged(R"({"protocolVersion":1,"cards":{}})", pocket::ParseResult::WrongFieldType);
  expectFailureLeavesDestinationUnchanged(bundleJson({}), pocket::ParseResult::EmptyCards);
}

TEST(PocketCardParserDocumentFailureTest, MoreThanEightCardsIsRejected) {
  const std::vector<std::string> cards(pocket::MAX_CARDS + 1, cardJson());
  expectFailureLeavesDestinationUnchanged(bundleJson(cards), pocket::ParseResult::TooManyCards);
}

TEST(PocketCardParserDocumentFailureTest, TrailingCommaInCardsArrayIsMalformedJson) {
  expectFailureLeavesDestinationUnchanged(
      R"({"protocolVersion":1,"cards":[{"label":"L","title":"T","subtitle":"S","lines":[]},]})",
      pocket::ParseResult::MalformedJson);
}

TEST(PocketCardParserDocumentFailureTest, DuplicateRootRequiredFieldsAreRejected) {
  expectFailureLeavesDestinationUnchanged(R"({"protocolVersion":1,"protocolVersion":1,"cards":[]})",
                                          pocket::ParseResult::DuplicateField);
  expectFailureLeavesDestinationUnchanged(
      "{\"protocolVersion\":1,\"cards\":[" + cardJson() + "],\"cards\":[" + cardJson() + "]}",
      pocket::ParseResult::DuplicateField);
}

TEST(PocketCardParserCardFailureTest, NonObjectCardIsRejected) {
  expectFailureLeavesDestinationUnchanged(R"({"protocolVersion":1,"cards":["card"]})",
                                          pocket::ParseResult::WrongFieldType);
}

TEST(PocketCardParserCardFailureTest, EveryMissingCardFieldIsRejected) {
  const std::vector<std::string> cards = {
      R"({"title":"T","subtitle":"S","lines":[]})",
      R"({"label":"L","subtitle":"S","lines":[]})",
      R"({"label":"L","title":"T","lines":[]})",
      R"({"label":"L","title":"T","subtitle":"S"})",
  };
  for (const auto& card : cards) {
    expectFailureLeavesDestinationUnchanged(bundleJson({card}), pocket::ParseResult::MissingRequiredField);
  }
}

TEST(PocketCardParserCardFailureTest, EveryRequiredCardFieldWrongTypeIsRejected) {
  const std::vector<std::string> cards = {
      R"({"label":1,"title":"T","subtitle":"S","lines":[]})",
      R"({"label":"L","title":1,"subtitle":"S","lines":[]})",
      R"({"label":"L","title":"T","subtitle":1,"lines":[]})",
      R"({"label":"L","title":"T","subtitle":"S","lines":{}})",
      R"({"label":"L","title":"T","subtitle":"S","lines":[1]})",
  };
  for (const auto& card : cards) {
    expectFailureLeavesDestinationUnchanged(bundleJson({card}), pocket::ParseResult::WrongFieldType);
  }
}

TEST(PocketCardParserCardFailureTest, EveryDuplicateRequiredCardFieldIsRejected) {
  const std::vector<std::string> duplicateFields = {R"("label":"again")", R"("title":"again")", R"("subtitle":"again")",
                                                    R"("lines":[])"};
  for (const auto& duplicate : duplicateFields) {
    std::string card = cardJson();
    card.insert(card.size() - 1, "," + duplicate);
    expectFailureLeavesDestinationUnchanged(bundleJson({card}), pocket::ParseResult::DuplicateField);
  }
}

TEST(PocketCardParserCardFailureTest, MoreThanSixLinesIsRejected) {
  const std::vector<std::string> lines(pocket::MAX_LINES_PER_CARD + 1, "line");
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson("L", "T", "S", lines)}),
                                          pocket::ParseResult::TooManyLines);
}

TEST(PocketCardParserCardFailureTest, TrailingCommaInLinesArrayIsMalformedJson) {
  expectFailureLeavesDestinationUnchanged(
      R"({"protocolVersion":1,"cards":[{"label":"L","title":"T","subtitle":"S","lines":["x",]}]})",
      pocket::ParseResult::MalformedJson);
}

TEST(PocketCardParserCardFailureTest, EveryOversizedTextFieldIsRejectedWithoutTruncation) {
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson(std::string(pocket::MAX_LABEL_BYTES + 1, 'L'))}),
                                          pocket::ParseResult::TextTooLong);
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson("L", std::string(pocket::MAX_TITLE_BYTES + 1, 'T'))}),
                                          pocket::ParseResult::TextTooLong);
  expectFailureLeavesDestinationUnchanged(
      bundleJson({cardJson("L", "T", std::string(pocket::MAX_SUBTITLE_BYTES + 1, 'S'))}),
      pocket::ParseResult::TextTooLong);
  expectFailureLeavesDestinationUnchanged(
      bundleJson({cardJson("L", "T", "S", {std::string(pocket::MAX_LINE_BYTES + 1, 'X')})}),
      pocket::ParseResult::TextTooLong);
}

TEST(PocketCardParserCardFailureTest, InvalidUtf8InEveryTextFieldIsRejected) {
  const std::string invalid = withInvalidByte();
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson(invalid)}), pocket::ParseResult::InvalidUtf8);
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson("L", invalid)}), pocket::ParseResult::InvalidUtf8);
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson("L", "T", invalid)}), pocket::ParseResult::InvalidUtf8);
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson("L", "T", "S", {invalid})}),
                                          pocket::ParseResult::InvalidUtf8);
}

TEST(PocketCardParserCardFailureTest, IncompleteAndOverlongUtf8AreRejected) {
  const std::string incomplete(1, static_cast<char>(0xC3));
  const std::string overlong = std::string(1, static_cast<char>(0xC0)) + std::string(1, static_cast<char>(0xAF));
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson(incomplete)}), pocket::ParseResult::InvalidUtf8);
  expectFailureLeavesDestinationUnchanged(bundleJson({cardJson(overlong)}), pocket::ParseResult::InvalidUtf8);
}

TEST(PocketCardParserCardFailureTest, InvalidUnicodeSurrogatesAreRejected) {
  expectFailureLeavesDestinationUnchanged(
      R"({"protocolVersion":1,"cards":[{"label":"\uD800","title":"T","subtitle":"S","lines":[]}]})",
      pocket::ParseResult::InvalidUtf8);
  expectFailureLeavesDestinationUnchanged(
      R"({"protocolVersion":1,"cards":[{"label":"\uDC00","title":"T","subtitle":"S","lines":[]}]})",
      pocket::ParseResult::InvalidUtf8);
}

TEST(PocketCardParserSafetyTest, FailedParsingLeavesPopulatedDestinationUnchanged) {
  pocket::CardBundle destination;
  ASSERT_EQ(parse(bundleJson({cardJson("Original", "Title", "Subtitle", {"Line"})}), destination),
            pocket::ParseResult::Success);
  const pocket::CardBundle original = destination;

  EXPECT_EQ(parse(R"({"protocolVersion":1,"cards":[]})", destination), pocket::ParseResult::EmptyCards);
  EXPECT_EQ(std::memcmp(&destination, &original, sizeof(destination)), 0);
}

TEST(PocketCardParserSafetyTest, NavigationUsesParsedCardCount) {
  pocket::CardBundle bundle;
  ASSERT_EQ(parse(bundleJson({cardJson("First"), cardJson("Second")}), bundle), pocket::ParseResult::Success);
  pocket::CardSelection selection(bundle.cardCount);

  EXPECT_EQ(selection.cardCount(), 2U);
  EXPECT_TRUE(selection.selectNext());
  EXPECT_FALSE(selection.selectNext());
  EXPECT_EQ(selection.index(), 1U);
}

TEST(PocketCardParserSafetyTest, OneParsedCardDisablesBothNavigationDirections) {
  pocket::CardBundle bundle;
  ASSERT_EQ(parse(bundleJson({cardJson()}), bundle), pocket::ParseResult::Success);
  pocket::CardSelection selection(bundle.cardCount);

  EXPECT_FALSE(selection.canSelectPrevious());
  EXPECT_FALSE(selection.canSelectNext());
}

TEST(PocketCardParserSafetyTest, ParsedOutOfRangeAccessIsBounded) {
  pocket::CardBundle bundle;
  ASSERT_EQ(parse(bundleJson({cardJson("First"), cardJson("Second")}), bundle), pocket::ParseResult::Success);

  EXPECT_EQ(&bundle.cardAt(99), &bundle.cards[0]);
  EXPECT_STREQ(bundle.cardAt(99).label, "First");
}

TEST(PocketCardParserSafetyTest, ErrorNamesContainOnlyCategories) {
  EXPECT_STREQ(pocket::parseResultName(pocket::ParseResult::InvalidUtf8), "invalid_utf8");
  EXPECT_STREQ(pocket::parseResultName(pocket::ParseResult::MalformedJson), "malformed_json");
}
