#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

inline constexpr uint16_t CLOCK_MIN_YEAR = 2000;
inline constexpr uint16_t CLOCK_MAX_YEAR = 2199;
inline constexpr size_t DS3231_DATE_TIME_REGISTER_BYTES = 7;
inline constexpr uint8_t DS3231_STATUS_OSF = 0x80;

// DS3231 day-of-week values are user-defined but must be sequential. CrossPoint uses
// 1 = Sunday through 7 = Saturday, matching std::tm::tm_wday + 1.
struct ClockDateTime {
  uint16_t year = CLOCK_MIN_YEAR;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t dayOfWeek = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
};

enum class ClockDateTimeResult : uint8_t {
  Success,
  InvalidBcd,
  InvalidDateTime,
  OscillatorStopped,
  OffsetOutOfRange,
};

bool isClockLeapYear(uint16_t year);
uint8_t clockDaysInMonth(uint16_t year, uint8_t month);
bool isValidClockDateTime(const ClockDateTime& dateTime);

ClockDateTimeResult decodeDs3231DateTime(const uint8_t (&registers)[DS3231_DATE_TIME_REGISTER_BYTES], uint8_t status,
                                         ClockDateTime& destination);
ClockDateTimeResult encodeDs3231DateTime(const ClockDateTime& dateTime,
                                         uint8_t (&registers)[DS3231_DATE_TIME_REGISTER_BYTES]);
ClockDateTimeResult applyClockUtcOffset(const ClockDateTime& utc, int offsetQuarterHours, ClockDateTime& destination);
ClockDateTimeResult clockDateTimeFromUtcTm(const std::tm& utc, ClockDateTime& destination);

bool formatClockTime(uint8_t hour, uint8_t minute, char* buffer, size_t bufferSize, bool use12Hour);
