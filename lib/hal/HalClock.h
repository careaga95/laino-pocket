#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalClockDateTime.h"
#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable ClockDateTime _cachedDateTime;
  mutable bool _hasCachedDateTime = false;

 public:
  enum class ReadResult : uint8_t {
    Fresh,
    Cached,
    Unavailable,
    ReadFailure,
    OscillatorStopped,
    InvalidData,
  };

  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // True if the DS3231 RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Compatibility wrapper: may use a clearly tracked cached value after an I2C read failure.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Read the complete UTC date and time. Fresh data is published only after the seven
  // timekeeping registers and OSF status validate. Cached is distinct from a fresh read.
  ReadResult getDateTimeUtc(ClockDateTime& dateTime) const;

  // Apply the persisted quarter-hour offset convention (48 = UTC, 0 = UTC-12, 104 = UTC+14)
  // to the complete date and time, including day/month/year rollover.
  ReadResult getDateTime(ClockDateTime& dateTime, uint8_t utcOffsetQuarterHoursBiased = 48) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync the DS3231 RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

  // Write a complete UTC value in one 0x00-0x06 transfer. OSF is cleared only after that
  // transfer succeeds and the status register can be safely updated.
  bool writeDateTimeUtc(const ClockDateTime& dateTime);

 private:
  ReadResult cachedOr(ReadResult failure, ClockDateTime& dateTime) const;
  bool readRegister(uint8_t address, uint8_t& value) const;
  bool writeRegister(uint8_t address, uint8_t value);
};
