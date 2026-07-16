#include <gtest/gtest.h>

#include <cstring>

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
