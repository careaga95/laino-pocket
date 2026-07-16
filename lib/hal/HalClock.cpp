#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

HalClock halClock;  // Singleton instance

namespace {

constexpr uint8_t DS3231_STATUS_REG = 0x0F;

bool readSucceeded(const HalClock::ReadResult result) {
  return result == HalClock::ReadResult::Fresh || result == HalClock::ReadResult::Cached;
}

}  // namespace

void HalClock::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  // I2C is already initialised by HalPowerManager::begin() for X3.
  uint8_t seconds = 0;
  if (!readRegister(DS3231_SEC_REG, seconds)) {
    LOG_INF("CLK", "DS3231 RTC not found");
    _available = false;
    return;
  }

  _available = true;
  LOG_INF("CLK", "DS3231 RTC found");

  ClockDateTime initial;
  const ReadResult result = getDateTimeUtc(initial);
  if (result == ReadResult::OscillatorStopped) LOG_INF("CLK", "DS3231 time is not reliable (OSF set)");
}

bool HalClock::readRegister(const uint8_t address, uint8_t& value) const {
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(address);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(I2C_ADDR_DS3231, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) != 1 ||
      Wire.available() < 1) {
    while (Wire.available()) Wire.read();
    return false;
  }
  value = static_cast<uint8_t>(Wire.read());
  return true;
}

bool HalClock::writeRegister(const uint8_t address, const uint8_t value) {
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(address);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

HalClock::ReadResult HalClock::cachedOr(const ReadResult failure, ClockDateTime& dateTime) const {
  if (!_hasCachedDateTime) return failure;
  dateTime = _cachedDateTime;
  return ReadResult::Cached;
}

HalClock::ReadResult HalClock::getDateTimeUtc(ClockDateTime& dateTime) const {
  if (!_available) return ReadResult::Unavailable;

  uint8_t registers[DS3231_DATE_TIME_REGISTER_BYTES];
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) return cachedOr(ReadResult::ReadFailure, dateTime);
  if (Wire.requestFrom(I2C_ADDR_DS3231, static_cast<uint8_t>(sizeof(registers)), static_cast<uint8_t>(true)) !=
          sizeof(registers) ||
      Wire.available() < static_cast<int>(sizeof(registers))) {
    while (Wire.available()) Wire.read();
    return cachedOr(ReadResult::ReadFailure, dateTime);
  }
  for (uint8_t& value : registers) value = static_cast<uint8_t>(Wire.read());

  uint8_t status = 0;
  if (!readRegister(DS3231_STATUS_REG, status)) return cachedOr(ReadResult::ReadFailure, dateTime);

  ClockDateTime candidate;
  const ClockDateTimeResult decoded = decodeDs3231DateTime(registers, status, candidate);
  if (decoded == ClockDateTimeResult::OscillatorStopped) return ReadResult::OscillatorStopped;
  if (decoded != ClockDateTimeResult::Success) return ReadResult::InvalidData;

  _cachedDateTime = candidate;
  _hasCachedDateTime = true;
  dateTime = candidate;
  return ReadResult::Fresh;
}

HalClock::ReadResult HalClock::getDateTime(ClockDateTime& dateTime, uint8_t utcOffsetQuarterHoursBiased) const {
  ClockDateTime utc;
  const ReadResult readResult = getDateTimeUtc(utc);
  if (!readSucceeded(readResult)) return readResult;
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  ClockDateTime adjusted;
  if (applyClockUtcOffset(utc, static_cast<int>(utcOffsetQuarterHoursBiased) - 48, adjusted) !=
      ClockDateTimeResult::Success) {
    return ReadResult::InvalidData;
  }
  dateTime = adjusted;
  return readResult;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  ClockDateTime dateTime;
  const ReadResult result = getDateTimeUtc(dateTime);
  if (!readSucceeded(result)) return false;
  hour = dateTime.hour;
  minute = dateTime.minute;
  return true;
}

bool HalClock::formatTime(char* buffer, const size_t bufferSize, const uint8_t utcOffsetQuarterHoursBiased,
                          const bool use12Hour) const {
  ClockDateTime dateTime;
  const ReadResult result = getDateTime(dateTime, utcOffsetQuarterHoursBiased);
  if (!readSucceeded(result)) return false;
  return formatClockTime(dateTime.hour, dateTime.minute, buffer, bufferSize, use12Hour);
}

bool HalClock::writeDateTimeUtc(const ClockDateTime& dateTime) {
  if (!_available) return false;
  uint8_t registers[DS3231_DATE_TIME_REGISTER_BYTES];
  if (encodeDs3231DateTime(dateTime, registers) != ClockDateTimeResult::Success) return false;

  // Writing seconds resets the countdown chain; the remaining registers follow in the same
  // transaction, comfortably inside the datasheet's one-second coherence requirement.
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  for (const uint8_t value : registers) Wire.write(value);
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write date and time to DS3231");
    return false;
  }

  uint8_t status = 0;
  if (!readRegister(DS3231_STATUS_REG, status)) return false;
  if ((status & DS3231_STATUS_OSF) != 0 && !writeRegister(DS3231_STATUS_REG, status & ~DS3231_STATUS_OSF)) {
    LOG_ERR("CLK", "Failed to clear DS3231 OSF");
    return false;
  }

  _cachedDateTime = dateTime;
  _hasCachedDateTime = true;
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; ++i) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      const time_t now = time(nullptr);
      std::tm timeInfo;
      if (gmtime_r(&now, &timeInfo) == nullptr) return false;
      ClockDateTime utc;
      if (clockDateTimeFromUtcTm(timeInfo, utc) != ClockDateTimeResult::Success) return false;
      if (!writeDateTimeUtc(utc)) return false;
      LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", utc.year, utc.month, utc.day, utc.hour, utc.minute,
              utc.second);
      return true;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
