#include "HalClockDateTime.h"

#include <cstdio>
#include <cstring>

namespace {

bool decodeBcd(const uint8_t value, uint8_t& destination) {
  const uint8_t high = value >> 4U;
  const uint8_t low = value & 0x0FU;
  if (high > 9 || low > 9) return false;
  destination = static_cast<uint8_t>(high * 10U + low);
  return true;
}

uint8_t encodeBcd(const uint8_t value) { return static_cast<uint8_t>(((value / 10U) << 4U) | (value % 10U)); }

bool incrementDate(ClockDateTime& dateTime) {
  if (dateTime.day < clockDaysInMonth(dateTime.year, dateTime.month)) {
    ++dateTime.day;
  } else {
    dateTime.day = 1;
    if (dateTime.month < 12) {
      ++dateTime.month;
    } else {
      if (dateTime.year == CLOCK_MAX_YEAR) return false;
      dateTime.month = 1;
      ++dateTime.year;
    }
  }
  dateTime.dayOfWeek = dateTime.dayOfWeek == 7 ? 1 : static_cast<uint8_t>(dateTime.dayOfWeek + 1);
  return true;
}

bool decrementDate(ClockDateTime& dateTime) {
  if (dateTime.day > 1) {
    --dateTime.day;
  } else {
    if (dateTime.month > 1) {
      --dateTime.month;
    } else {
      if (dateTime.year == CLOCK_MIN_YEAR) return false;
      dateTime.month = 12;
      --dateTime.year;
    }
    dateTime.day = clockDaysInMonth(dateTime.year, dateTime.month);
  }
  dateTime.dayOfWeek = dateTime.dayOfWeek == 1 ? 7 : static_cast<uint8_t>(dateTime.dayOfWeek - 1);
  return true;
}

}  // namespace

bool isClockLeapYear(const uint16_t year) { return year % 4U == 0 && (year % 100U != 0 || year % 400U == 0); }

uint8_t clockDaysInMonth(const uint16_t year, const uint8_t month) {
  constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  return month == 2 && isClockLeapYear(year) ? 29 : DAYS[month - 1];
}

bool isValidClockDateTime(const ClockDateTime& dateTime) {
  if (dateTime.year < CLOCK_MIN_YEAR || dateTime.year > CLOCK_MAX_YEAR) return false;
  if (dateTime.month < 1 || dateTime.month > 12) return false;
  if (dateTime.day < 1 || dateTime.day > clockDaysInMonth(dateTime.year, dateTime.month)) return false;
  if (dateTime.dayOfWeek < 1 || dateTime.dayOfWeek > 7) return false;
  return dateTime.hour < 24 && dateTime.minute < 60 && dateTime.second < 60;
}

ClockDateTimeResult decodeDs3231DateTime(const uint8_t (&registers)[DS3231_DATE_TIME_REGISTER_BYTES],
                                         const uint8_t status, ClockDateTime& destination) {
  if ((status & DS3231_STATUS_OSF) != 0) return ClockDateTimeResult::OscillatorStopped;
  if ((registers[0] & 0x80U) != 0 || (registers[1] & 0x80U) != 0 || (registers[2] & 0x80U) != 0 ||
      (registers[3] & 0xF8U) != 0 || (registers[4] & 0xC0U) != 0 || (registers[5] & 0x60U) != 0) {
    return ClockDateTimeResult::InvalidBcd;
  }

  ClockDateTime candidate;
  uint8_t year = 0;
  if (!decodeBcd(registers[0] & 0x7FU, candidate.second) || !decodeBcd(registers[1] & 0x7FU, candidate.minute) ||
      !decodeBcd(registers[4] & 0x3FU, candidate.day) || !decodeBcd(registers[5] & 0x1FU, candidate.month) ||
      !decodeBcd(registers[6], year)) {
    return ClockDateTimeResult::InvalidBcd;
  }

  if ((registers[2] & 0x40U) != 0) {
    uint8_t hour12 = 0;
    if (!decodeBcd(registers[2] & 0x1FU, hour12)) return ClockDateTimeResult::InvalidBcd;
    if (hour12 < 1 || hour12 > 12) return ClockDateTimeResult::InvalidDateTime;
    const bool pm = (registers[2] & 0x20U) != 0;
    candidate.hour = static_cast<uint8_t>((hour12 % 12U) + (pm ? 12U : 0U));
  } else {
    if (!decodeBcd(registers[2] & 0x3FU, candidate.hour)) return ClockDateTimeResult::InvalidBcd;
  }

  candidate.dayOfWeek = registers[3] & 0x07U;
  candidate.year = static_cast<uint16_t>(CLOCK_MIN_YEAR + year + ((registers[5] & 0x80U) != 0 ? 100U : 0U));
  if (!isValidClockDateTime(candidate)) return ClockDateTimeResult::InvalidDateTime;
  destination = candidate;
  return ClockDateTimeResult::Success;
}

ClockDateTimeResult encodeDs3231DateTime(const ClockDateTime& dateTime,
                                         uint8_t (&registers)[DS3231_DATE_TIME_REGISTER_BYTES]) {
  if (!isValidClockDateTime(dateTime)) return ClockDateTimeResult::InvalidDateTime;
  uint8_t candidate[DS3231_DATE_TIME_REGISTER_BYTES];
  candidate[0] = encodeBcd(dateTime.second);
  candidate[1] = encodeBcd(dateTime.minute);
  candidate[2] = encodeBcd(dateTime.hour);  // Always write the unambiguous 24-hour form.
  candidate[3] = dateTime.dayOfWeek;
  candidate[4] = encodeBcd(dateTime.day);
  candidate[5] = static_cast<uint8_t>(encodeBcd(dateTime.month) | (dateTime.year >= 2100 ? 0x80U : 0U));
  candidate[6] = encodeBcd(static_cast<uint8_t>(dateTime.year % 100U));
  std::memcpy(registers, candidate, sizeof(candidate));
  return ClockDateTimeResult::Success;
}

ClockDateTimeResult applyClockUtcOffset(const ClockDateTime& utc, const int offsetQuarterHours,
                                        ClockDateTime& destination) {
  if (!isValidClockDateTime(utc)) return ClockDateTimeResult::InvalidDateTime;
  if (offsetQuarterHours < -48 || offsetQuarterHours > 56) return ClockDateTimeResult::OffsetOutOfRange;

  ClockDateTime candidate = utc;
  int totalMinutes = static_cast<int>(candidate.hour) * 60 + candidate.minute + offsetQuarterHours * 15;
  while (totalMinutes < 0) {
    if (!decrementDate(candidate)) return ClockDateTimeResult::OffsetOutOfRange;
    totalMinutes += 1440;
  }
  while (totalMinutes >= 1440) {
    if (!incrementDate(candidate)) return ClockDateTimeResult::OffsetOutOfRange;
    totalMinutes -= 1440;
  }
  candidate.hour = static_cast<uint8_t>(totalMinutes / 60);
  candidate.minute = static_cast<uint8_t>(totalMinutes % 60);
  destination = candidate;
  return ClockDateTimeResult::Success;
}

ClockDateTimeResult clockDateTimeFromUtcTm(const std::tm& utc, ClockDateTime& destination) {
  ClockDateTime candidate;
  const int year = utc.tm_year + 1900;
  if (year < CLOCK_MIN_YEAR || year > CLOCK_MAX_YEAR || utc.tm_mon < 0 || utc.tm_mon > 11 || utc.tm_mday < 1 ||
      utc.tm_mday > 31 || utc.tm_wday < 0 || utc.tm_wday > 6 || utc.tm_hour < 0 || utc.tm_hour > 23 || utc.tm_min < 0 ||
      utc.tm_min > 59 || utc.tm_sec < 0 || utc.tm_sec > 59) {
    return ClockDateTimeResult::InvalidDateTime;
  }
  candidate.year = static_cast<uint16_t>(year);
  candidate.month = static_cast<uint8_t>(utc.tm_mon + 1);
  candidate.day = static_cast<uint8_t>(utc.tm_mday);
  candidate.dayOfWeek = static_cast<uint8_t>(utc.tm_wday + 1);
  candidate.hour = static_cast<uint8_t>(utc.tm_hour);
  candidate.minute = static_cast<uint8_t>(utc.tm_min);
  candidate.second = static_cast<uint8_t>(utc.tm_sec);
  if (!isValidClockDateTime(candidate)) return ClockDateTimeResult::InvalidDateTime;
  destination = candidate;
  return ClockDateTimeResult::Success;
}

bool formatClockTime(const uint8_t hour, const uint8_t minute, char* buffer, const size_t bufferSize,
                     const bool use12Hour) {
  if (buffer == nullptr || hour >= 24 || minute >= 60 || bufferSize < (use12Hour ? 9U : 6U)) return false;
  char formatted[9];
  if (use12Hour) {
    const bool pm = hour >= 12;
    int hour12 = hour % 12;
    if (hour12 == 0) hour12 = 12;
    std::snprintf(formatted, sizeof(formatted), "%d:%02d %s", hour12, minute, pm ? "PM" : "AM");
  } else {
    std::snprintf(formatted, sizeof(formatted), "%02d:%02d", hour, minute);
  }
  std::memcpy(buffer, formatted, use12Hour ? 9U : 6U);
  return true;
}
