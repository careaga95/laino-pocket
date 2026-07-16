#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "HalClockDateTime.h"

namespace {

using Registers = std::array<uint8_t, DS3231_DATE_TIME_REGISTER_BYTES>;

ClockDateTime knownDateTime() { return ClockDateTime{2026, 7, 16, 5, 21, 34, 56}; }

Registers knownRegisters() { return {0x56, 0x34, 0x21, 0x05, 0x16, 0x07, 0x26}; }

ClockDateTimeResult decode(const Registers& source, const uint8_t status, ClockDateTime& destination) {
  uint8_t registers[DS3231_DATE_TIME_REGISTER_BYTES];
  std::memcpy(registers, source.data(), sizeof(registers));
  return decodeDs3231DateTime(registers, status, destination);
}

Registers encode(const ClockDateTime& dateTime) {
  uint8_t registers[DS3231_DATE_TIME_REGISTER_BYTES]{};
  EXPECT_EQ(encodeDs3231DateTime(dateTime, registers), ClockDateTimeResult::Success);
  Registers result;
  std::memcpy(result.data(), registers, sizeof(registers));
  return result;
}

void expectDateTime(const ClockDateTime& actual, const ClockDateTime& expected) {
  EXPECT_EQ(actual.year, expected.year);
  EXPECT_EQ(actual.month, expected.month);
  EXPECT_EQ(actual.day, expected.day);
  EXPECT_EQ(actual.dayOfWeek, expected.dayOfWeek);
  EXPECT_EQ(actual.hour, expected.hour);
  EXPECT_EQ(actual.minute, expected.minute);
  EXPECT_EQ(actual.second, expected.second);
}

}  // namespace

TEST(HalClockDateTimeFormatTest, FixedRepresentationIsEightBytes) { EXPECT_EQ(sizeof(ClockDateTime), 8U); }

TEST(HalClockDateTimeFormatTest, DecodesKnown24HourRegisters) {
  ClockDateTime result;
  EXPECT_EQ(decode(knownRegisters(), 0, result), ClockDateTimeResult::Success);
  expectDateTime(result, knownDateTime());
}

TEST(HalClockDateTimeFormatTest, EncodesKnown24HourRegisters) { EXPECT_EQ(encode(knownDateTime()), knownRegisters()); }

TEST(HalClockDateTimeFormatTest, Decodes12HourAmAndPm) {
  Registers midnight = knownRegisters();
  midnight[2] = 0x52;  // 12-hour mode, 12 AM
  ClockDateTime result;
  ASSERT_EQ(decode(midnight, 0, result), ClockDateTimeResult::Success);
  EXPECT_EQ(result.hour, 0U);

  Registers afternoon = knownRegisters();
  afternoon[2] = 0x63;  // 12-hour mode, 3 PM
  ASSERT_EQ(decode(afternoon, 0, result), ClockDateTimeResult::Success);
  EXPECT_EQ(result.hour, 15U);
}

TEST(HalClockDateTimeFormatTest, RejectsInvalidBcdNibbles) {
  for (const size_t index : {0U, 1U, 4U, 5U, 6U}) {
    Registers registers = knownRegisters();
    registers[index] = 0x1A;
    ClockDateTime result;
    EXPECT_EQ(decode(registers, 0, result), ClockDateTimeResult::InvalidBcd) << index;
  }
  Registers hours = knownRegisters();
  hours[2] = 0x2A;
  ClockDateTime result;
  EXPECT_EQ(decode(hours, 0, result), ClockDateTimeResult::InvalidBcd);
}

TEST(HalClockDateTimeFormatTest, RejectsInvalidHourMonthAndDay) {
  for (const auto& registers :
       {Registers{0x00, 0x00, 0x24, 0x01, 0x01, 0x01, 0x26}, Registers{0x00, 0x00, 0x12, 0x01, 0x01, 0x13, 0x26},
        Registers{0x00, 0x00, 0x12, 0x01, 0x31, 0x04, 0x26}}) {
    ClockDateTime result;
    EXPECT_EQ(decode(registers, 0, result), ClockDateTimeResult::InvalidDateTime);
  }
}

TEST(HalClockDateTimeFormatTest, CenturyBitSelects2100To2199) {
  Registers registers = knownRegisters();
  registers[5] |= 0x80;
  registers[6] = 0x04;
  ClockDateTime result;
  ASSERT_EQ(decode(registers, 0, result), ClockDateTimeResult::Success);
  EXPECT_EQ(result.year, 2104U);
  EXPECT_EQ(encode(result)[5], 0x87U);
}

TEST(HalClockDateTimeFormatTest, OsfRejectsDataAndPreservesDestination) {
  ClockDateTime destination{2030, 3, 4, 2, 5, 6, 7};
  const ClockDateTime original = destination;
  EXPECT_EQ(decode(knownRegisters(), DS3231_STATUS_OSF, destination), ClockDateTimeResult::OscillatorStopped);
  expectDateTime(destination, original);
}

TEST(HalClockCalendarTest, LeapYearRulesIncludeCenturyException) {
  EXPECT_TRUE(isClockLeapYear(2000));
  EXPECT_TRUE(isClockLeapYear(2024));
  EXPECT_FALSE(isClockLeapYear(2100));
  EXPECT_EQ(clockDaysInMonth(2024, 2), 29U);
  EXPECT_EQ(clockDaysInMonth(2100, 2), 28U);
}

TEST(HalClockCalendarTest, RejectsFebruary29InNonLeapYear) {
  ClockDateTime invalid{2025, 2, 29, 7, 12, 0, 0};
  uint8_t registers[DS3231_DATE_TIME_REGISTER_BYTES]{};
  EXPECT_EQ(encodeDs3231DateTime(invalid, registers), ClockDateTimeResult::InvalidDateTime);
}

TEST(HalClockOffsetTest, PositiveOffsetCrossesToNextDay) {
  ClockDateTime utc{2026, 7, 16, 5, 23, 45, 12};
  ClockDateTime local;
  ASSERT_EQ(applyClockUtcOffset(utc, 4, local), ClockDateTimeResult::Success);
  expectDateTime(local, ClockDateTime{2026, 7, 17, 6, 0, 45, 12});
}

TEST(HalClockOffsetTest, NegativeOffsetCrossesToPreviousDay) {
  ClockDateTime utc{2026, 7, 16, 5, 0, 15, 12};
  ClockDateTime local;
  ASSERT_EQ(applyClockUtcOffset(utc, -4, local), ClockDateTimeResult::Success);
  expectDateTime(local, ClockDateTime{2026, 7, 15, 4, 23, 15, 12});
}

TEST(HalClockOffsetTest, PositiveOffsetCrossesMonth) {
  ClockDateTime utc{2026, 4, 30, 5, 23, 30, 0};
  ClockDateTime local;
  ASSERT_EQ(applyClockUtcOffset(utc, 4, local), ClockDateTimeResult::Success);
  expectDateTime(local, ClockDateTime{2026, 5, 1, 6, 0, 30, 0});
}

TEST(HalClockOffsetTest, PositiveOffsetCrossesYear) {
  ClockDateTime utc{2026, 12, 31, 5, 23, 30, 0};
  ClockDateTime local;
  ASSERT_EQ(applyClockUtcOffset(utc, 4, local), ClockDateTimeResult::Success);
  expectDateTime(local, ClockDateTime{2027, 1, 1, 6, 0, 30, 0});
}

TEST(HalClockOffsetTest, HandlesUtcMinus12AndUtcPlus14Limits) {
  ClockDateTime utc{2026, 7, 16, 5, 12, 0, 0};
  ClockDateTime local;
  ASSERT_EQ(applyClockUtcOffset(utc, -48, local), ClockDateTimeResult::Success);
  expectDateTime(local, ClockDateTime{2026, 7, 16, 5, 0, 0, 0});
  ASSERT_EQ(applyClockUtcOffset(utc, 56, local), ClockDateTimeResult::Success);
  expectDateTime(local, ClockDateTime{2026, 7, 17, 6, 2, 0, 0});
}

TEST(HalClockOffsetTest, RejectsOffsetOutsideSupportedRangeWithoutPublishing) {
  const ClockDateTime utc = knownDateTime();
  ClockDateTime destination{2030, 1, 2, 4, 3, 4, 5};
  const ClockDateTime original = destination;
  EXPECT_EQ(applyClockUtcOffset(utc, 57, destination), ClockDateTimeResult::OffsetOutOfRange);
  expectDateTime(destination, original);
}

TEST(HalClockOffsetTest, RejectsRolloverBeyondRepresentableYearsWithoutPublishing) {
  ClockDateTime utc{2199, 12, 31, 5, 23, 59, 59};
  ClockDateTime destination{2030, 1, 2, 4, 3, 4, 5};
  const ClockDateTime original = destination;
  EXPECT_EQ(applyClockUtcOffset(utc, 1, destination), ClockDateTimeResult::OffsetOutOfRange);
  expectDateTime(destination, original);
}

TEST(HalClockFormatCompatibilityTest, PreservesExisting24HourFormatting) {
  char buffer[9] = {};
  ASSERT_TRUE(formatClockTime(0, 5, buffer, sizeof(buffer), false));
  EXPECT_STREQ(buffer, "00:05");
  ASSERT_TRUE(formatClockTime(23, 59, buffer, sizeof(buffer), false));
  EXPECT_STREQ(buffer, "23:59");
}

TEST(HalClockFormatCompatibilityTest, PreservesExisting12HourFormatting) {
  char buffer[9] = {};
  ASSERT_TRUE(formatClockTime(0, 5, buffer, sizeof(buffer), true));
  EXPECT_STREQ(buffer, "12:05 AM");
  ASSERT_TRUE(formatClockTime(12, 30, buffer, sizeof(buffer), true));
  EXPECT_STREQ(buffer, "12:30 PM");
  ASSERT_TRUE(formatClockTime(15, 7, buffer, sizeof(buffer), true));
  EXPECT_STREQ(buffer, "3:07 PM");
}

TEST(HalClockFormatCompatibilityTest, InvalidInputDoesNotModifyOutput) {
  char buffer[9] = "original";
  EXPECT_FALSE(formatClockTime(24, 0, buffer, sizeof(buffer), false));
  EXPECT_STREQ(buffer, "original");
}

TEST(HalClockNtpTest, UtcTmBuildsAllSevenRtcRegisters) {
  std::tm timeInfo{};
  timeInfo.tm_year = 126;
  timeInfo.tm_mon = 6;
  timeInfo.tm_mday = 16;
  timeInfo.tm_wday = 4;
  timeInfo.tm_hour = 21;
  timeInfo.tm_min = 34;
  timeInfo.tm_sec = 56;
  ClockDateTime utc;
  ASSERT_EQ(clockDateTimeFromUtcTm(timeInfo, utc), ClockDateTimeResult::Success);
  expectDateTime(utc, knownDateTime());
  EXPECT_EQ(encode(utc), knownRegisters());
}

TEST(HalClockNtpTest, InvalidUtcTmDoesNotPublish) {
  std::tm timeInfo{};
  timeInfo.tm_year = 126;
  timeInfo.tm_mon = 1;
  timeInfo.tm_mday = 30;
  timeInfo.tm_wday = 1;
  ClockDateTime destination = knownDateTime();
  const ClockDateTime original = destination;
  EXPECT_EQ(clockDateTimeFromUtcTm(timeInfo, destination), ClockDateTimeResult::InvalidDateTime);
  expectDateTime(destination, original);
}
