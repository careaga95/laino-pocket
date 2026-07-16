#include <gtest/gtest.h>

#include <cstring>

#include "PocketCard.h"
#include "PocketCardSelection.h"
#include "PocketText.h"

TEST(PocketCardSelectionTest, PreviousIsBoundedAtFirstCard) {
  pocket::CardSelection selection(3);

  EXPECT_FALSE(selection.selectPrevious());
  EXPECT_EQ(selection.index(), 0U);
}

TEST(PocketCardSelectionTest, NextIsBoundedAtLastCard) {
  pocket::CardSelection selection(3);

  EXPECT_TRUE(selection.selectNext());
  EXPECT_TRUE(selection.selectNext());
  EXPECT_FALSE(selection.selectNext());
  EXPECT_EQ(selection.index(), 2U);
}

TEST(PocketCardSelectionTest, RepeatedNavigationNeverLeavesDeck) {
  pocket::CardSelection selection(3);

  for (int i = 0; i < 10; ++i) {
    selection.selectNext();
  }
  EXPECT_EQ(selection.index(), 2U);

  for (int i = 0; i < 10; ++i) {
    selection.selectPrevious();
  }
  EXPECT_EQ(selection.index(), 0U);
}

TEST(PocketTextTest, OversizedTextIsTruncatedWithEllipsis) {
  char buffer[16];
  const auto measureBytes = [](const char* text) { return static_cast<int>(std::strlen(text)); };

  const char* result = pocket::truncateToWidth("Review a very long line", 10, buffer, measureBytes);

  EXPECT_EQ(result, buffer);
  EXPECT_LE(measureBytes(result), 10);
  EXPECT_NE(std::strstr(result, "\xE2\x80\xA6"), nullptr);
}

TEST(PocketTextTest, Utf8TextEndsOnCodepointBoundary) {
  char buffer[10];
  const auto measureBytes = [](const char* text) { return static_cast<int>(std::strlen(text)); };

  const char* result = pocket::truncateToWidth("Español extendido", 8, buffer, measureBytes);

  EXPECT_EQ(result, buffer);
  EXPECT_EQ(utf8SafeTruncateBuffer(result, static_cast<int>(std::strlen(result))),
            static_cast<int>(std::strlen(result)));
}

TEST(PocketTextTest, MalformedAndIncompleteUtf8AreSafelyTruncated) {
  char malformedBuffer[12];
  char incompleteBuffer[12];
  const auto measureBytes = [](const char* text) { return static_cast<int>(std::strlen(text)); };
  const char malformed[] = {'a', 'b', static_cast<char>(0x80), 'c', 'd', '\0'};
  const char incomplete[] = {'a', 'b', 'c', 'd', static_cast<char>(0xC3), '\0'};

  const char* malformedResult = pocket::truncateToWidth(malformed, 4, malformedBuffer, measureBytes);
  const char* incompleteResult = pocket::truncateToWidth(incomplete, 3, incompleteBuffer, measureBytes);

  EXPECT_STREQ(malformedResult, "a\xE2\x80\xA6");
  EXPECT_STREQ(incompleteResult, "\xE2\x80\xA6");
}

TEST(PocketTextTest, ReturnsEmptyWhenEllipsisDoesNotFit) {
  char buffer[16];
  const auto measureBytes = [](const char* text) { return static_cast<int>(std::strlen(text)); };

  const char* result = pocket::truncateToWidth("Too long", 2, buffer, measureBytes);

  EXPECT_EQ(result, buffer);
  EXPECT_STREQ(result, "");
}

TEST(PocketTextTest, HandlesMissingEmptyAndNonPositiveInputs) {
  char buffer[16];
  int measureCalls = 0;
  const auto measureBytes = [&measureCalls](const char* text) {
    ++measureCalls;
    return static_cast<int>(std::strlen(text));
  };

  EXPECT_STREQ(pocket::truncateToWidth(nullptr, 10, buffer, measureBytes), "");
  EXPECT_STREQ(pocket::truncateToWidth("", 10, buffer, measureBytes), "");
  EXPECT_STREQ(pocket::truncateToWidth("Text", 0, buffer, measureBytes), "");
  EXPECT_STREQ(pocket::truncateToWidth("Text", -1, buffer, measureBytes), "");
  EXPECT_EQ(measureCalls, 0);
}

TEST(PocketCardTest, OutOfRangeIndexFallsBackToFirstFixture) {
  const pocket::Card& firstCard = pocket::cardAt(0);
  const pocket::Card& fallbackCard = pocket::cardAt(pocket::CARD_COUNT + 1);

  EXPECT_EQ(&fallbackCard, &firstCard);
}
