#include "PocketPairingProtocol.h"

#include <cstring>
#include <limits>

#include "PocketCredential.h"

namespace pocket {
namespace {

template <typename T>
class SecureScratch final {
 public:
  SecureScratch() = default;
  T value{};
  T* operator->() { return &value; }
  ~SecureScratch() { secureClear(&value, sizeof(value)); }

  SecureScratch(const SecureScratch&) = delete;
  SecureScratch& operator=(const SecureScratch&) = delete;
};

class JsonCursor {
 public:
  JsonCursor(const char* json, const std::size_t length)
      : cursor(json), end(json == nullptr ? nullptr : json + length) {}

  bool beginObject() {
    skipWhitespace();
    return consume('{');
  }

  bool nextObjectKey(bool& first, char* key, const std::size_t keySize, bool& finished) {
    skipWhitespace();
    if (consume('}')) {
      finished = true;
      return true;
    }
    if (!first && !consume(',')) return false;
    skipWhitespace();
    if (!parseString(key, keySize)) return false;
    skipWhitespace();
    if (!consume(':')) return false;
    first = false;
    finished = false;
    return true;
  }

  bool parseString(char* output, const std::size_t outputSize) {
    skipWhitespace();
    if (!consume('"') || output == nullptr || outputSize == 0) return false;
    std::size_t outputLength = 0;
    while (cursor < end) {
      const uint8_t byte = static_cast<uint8_t>(*cursor++);
      if (byte == '"') {
        output[outputLength] = '\0';
        return true;
      }
      if (byte < 0x20) return false;
      if (byte != '\\') {
        if (outputLength + 1 >= outputSize) return false;
        output[outputLength++] = static_cast<char>(byte);
        continue;
      }
      if (cursor >= end) return false;
      const char escaped = *cursor++;
      char decoded = '\0';
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          decoded = escaped;
          break;
        case 'b':
          decoded = '\b';
          break;
        case 'f':
          decoded = '\f';
          break;
        case 'n':
          decoded = '\n';
          break;
        case 'r':
          decoded = '\r';
          break;
        case 't':
          decoded = '\t';
          break;
        case 'u': {
          uint32_t codePoint = 0;
          if (!parseHex4(codePoint) || (codePoint >= 0xd800 && codePoint <= 0xdfff) || codePoint == 0) return false;
          if (!appendUtf8(codePoint, output, outputSize, outputLength)) return false;
          continue;
        }
        default:
          return false;
      }
      if (decoded == '\0' || outputLength + 1 >= outputSize) return false;
      output[outputLength++] = decoded;
    }
    return false;
  }

  bool parseUnsigned(uint32_t& value) {
    skipWhitespace();
    if (cursor >= end || *cursor < '0' || *cursor > '9') return false;
    if (*cursor == '0' && cursor + 1 < end && cursor[1] >= '0' && cursor[1] <= '9') return false;
    uint32_t parsed = 0;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
      const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
      if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10) return false;
      parsed = parsed * 10 + digit;
      ++cursor;
    }
    value = parsed;
    return true;
  }

  bool skipValue(const uint8_t depth = 0) {
    if (depth > 8) return false;
    skipWhitespace();
    if (cursor >= end) return false;
    if (*cursor == '"') {
      // A valid ignored string may be long, so scan it without materializing it.
      ++cursor;
      while (cursor < end) {
        const uint8_t byte = static_cast<uint8_t>(*cursor++);
        if (byte == '"') return true;
        if (byte < 0x20) return false;
        if (byte == '\\') {
          if (cursor >= end) return false;
          const char escaped = *cursor++;
          if (std::strchr("\"\\/bfnrt", escaped) != nullptr) continue;
          if (escaped != 'u') return false;
          uint32_t codePoint = 0;
          if (!parseHex4(codePoint)) return false;
        }
      }
      return false;
    }
    if (*cursor == '{') {
      ++cursor;
      bool first = true;
      while (true) {
        skipWhitespace();
        if (consume('}')) return true;
        if (!first && !consume(',')) return false;
        char key[33];
        if (!parseString(key, sizeof(key))) return false;
        skipWhitespace();
        if (!consume(':') || !skipValue(static_cast<uint8_t>(depth + 1))) return false;
        first = false;
      }
    }
    if (*cursor == '[') {
      ++cursor;
      bool first = true;
      while (true) {
        skipWhitespace();
        if (consume(']')) return true;
        if (!first && !consume(',')) return false;
        if (!skipValue(static_cast<uint8_t>(depth + 1))) return false;
        first = false;
      }
    }
    if (matchLiteral("true") || matchLiteral("false") || matchLiteral("null")) return true;
    return skipNumber();
  }

  bool complete() {
    skipWhitespace();
    return cursor == end;
  }

 private:
  const char* cursor;
  const char* end;

  void skipWhitespace() {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')) ++cursor;
  }

  bool consume(const char expected) {
    if (cursor >= end || *cursor != expected) return false;
    ++cursor;
    return true;
  }

  bool matchLiteral(const char* literal) {
    const std::size_t length = std::strlen(literal);
    if (static_cast<std::size_t>(end - cursor) < length || std::memcmp(cursor, literal, length) != 0) return false;
    cursor += length;
    return true;
  }

  bool skipNumber() {
    const char* start = cursor;
    if (cursor < end && *cursor == '-') ++cursor;
    if (cursor >= end) return false;
    if (*cursor == '0') {
      ++cursor;
    } else if (*cursor >= '1' && *cursor <= '9') {
      while (cursor < end && *cursor >= '0' && *cursor <= '9') ++cursor;
    } else {
      return false;
    }
    if (cursor < end && *cursor == '.') {
      ++cursor;
      if (cursor >= end || *cursor < '0' || *cursor > '9') return false;
      while (cursor < end && *cursor >= '0' && *cursor <= '9') ++cursor;
    }
    if (cursor < end && (*cursor == 'e' || *cursor == 'E')) {
      ++cursor;
      if (cursor < end && (*cursor == '+' || *cursor == '-')) ++cursor;
      if (cursor >= end || *cursor < '0' || *cursor > '9') return false;
      while (cursor < end && *cursor >= '0' && *cursor <= '9') ++cursor;
    }
    return cursor > start;
  }

  bool parseHex4(uint32_t& value) {
    if (static_cast<std::size_t>(end - cursor) < 4) return false;
    value = 0;
    for (uint8_t i = 0; i < 4; ++i) {
      const char c = *cursor++;
      uint8_t digit = 0;
      if (c >= '0' && c <= '9')
        digit = static_cast<uint8_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        digit = static_cast<uint8_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        digit = static_cast<uint8_t>(c - 'A' + 10);
      else
        return false;
      value = (value << 4) | digit;
    }
    return true;
  }

  static bool appendUtf8(const uint32_t codePoint, char* output, const std::size_t outputSize,
                         std::size_t& outputLength) {
    uint8_t encoded[3];
    std::size_t count = 0;
    if (codePoint <= 0x7f) {
      encoded[0] = static_cast<uint8_t>(codePoint);
      count = 1;
    } else if (codePoint <= 0x7ff) {
      encoded[0] = static_cast<uint8_t>(0xc0 | (codePoint >> 6));
      encoded[1] = static_cast<uint8_t>(0x80 | (codePoint & 0x3f));
      count = 2;
    } else {
      encoded[0] = static_cast<uint8_t>(0xe0 | (codePoint >> 12));
      encoded[1] = static_cast<uint8_t>(0x80 | ((codePoint >> 6) & 0x3f));
      encoded[2] = static_cast<uint8_t>(0x80 | (codePoint & 0x3f));
      count = 3;
    }
    if (outputLength + count >= outputSize) return false;
    for (std::size_t i = 0; i < count; ++i) output[outputLength++] = static_cast<char>(encoded[i]);
    return true;
  }
};

bool keyEquals(const char* key, const char* expected) { return std::strcmp(key, expected) == 0; }

bool parseProtocol(JsonCursor& parser, uint32_t& protocol) { return parser.parseUnsigned(protocol); }

bool validUuidText(const char* value) {
  uint8_t uuid[DEVICE_UUID_BYTES];
  return parseUuid(value, std::strlen(value), uuid, sizeof(uuid));
}

bool validUtf8(const char* value) {
  if (value == nullptr) return false;
  const auto* cursor = reinterpret_cast<const uint8_t*>(value);
  const auto* end = cursor + std::strlen(value);
  while (cursor < end) {
    const uint8_t first = *cursor++;
    if (first <= 0x7f) continue;
    uint32_t codePoint = 0;
    uint8_t continuationCount = 0;
    uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      codePoint = first & 0x1f;
      continuationCount = 1;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      codePoint = first & 0x0f;
      continuationCount = 2;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      codePoint = first & 0x07;
      continuationCount = 3;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (static_cast<std::size_t>(end - cursor) < continuationCount) return false;
    for (uint8_t i = 0; i < continuationCount; ++i) {
      const uint8_t next = *cursor++;
      if ((next & 0xc0) != 0x80) return false;
      codePoint = (codePoint << 6) | (next & 0x3f);
    }
    if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) return false;
  }
  return true;
}

template <typename ParseFields>
JsonParseResult parseObject(const char* json, const std::size_t length, const std::size_t maximum,
                            ParseFields&& parseFields) {
  if (length > maximum) return JsonParseResult::TooLarge;
  JsonCursor parser(json, length);
  if (!parser.beginObject()) return JsonParseResult::Malformed;
  bool first = true;
  bool finished = false;
  while (!finished) {
    char key[33];
    if (!parser.nextObjectKey(first, key, sizeof(key), finished)) return JsonParseResult::Malformed;
    if (!finished && !parseFields(parser, key)) return JsonParseResult::InvalidSchema;
  }
  return parser.complete() ? JsonParseResult::Success : JsonParseResult::Malformed;
}

}  // namespace

bool isBase64Url256(const char* value, const std::size_t length) {
  if (value == nullptr || length != DEVICE_CODE_BYTES) return false;
  for (std::size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      return false;
    }
  }
  // Base64URL without padding for 32 bytes has two zero padding bits.
  const char last = value[length - 1];
  const char* position = std::strchr("AEIMQUYcgkosw048", last);
  return position != nullptr;
}

bool isCanonicalUserCode(const char* value, const std::size_t length) {
  constexpr char ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  if (value == nullptr || length != USER_CODE_BYTES) return false;
  for (std::size_t i = 0; i < length; ++i) {
    if (std::strchr(ALPHABET, value[i]) == nullptr) return false;
  }
  return true;
}

JsonParseResult parsePairingStartResponse(const char* json, const std::size_t length, PairingStartResponse& response) {
  SecureScratch<PairingStartResponse> scratch;
  uint8_t fields = 0;
  uint32_t protocol = 0;
  const JsonParseResult result = parseObject(json, length, 384, [&](JsonCursor& parser, const char* key) {
    if (keyEquals(key, "protocol")) {
      if ((fields & 1) != 0 || !parseProtocol(parser, protocol)) return false;
      fields |= 1;
      return true;
    }
    if (keyEquals(key, "device_code")) {
      if ((fields & 2) != 0 || !parser.parseString(scratch->deviceCode, sizeof(scratch->deviceCode))) return false;
      fields |= 2;
      return true;
    }
    if (keyEquals(key, "user_code")) {
      if ((fields & 4) != 0 || !parser.parseString(scratch->userCode, sizeof(scratch->userCode))) return false;
      fields |= 4;
      return true;
    }
    uint32_t value = 0;
    if (keyEquals(key, "expires_in")) {
      if ((fields & 8) != 0 || !parser.parseUnsigned(value) || value > UINT16_MAX) return false;
      scratch->expiresIn = static_cast<uint16_t>(value);
      fields |= 8;
      return true;
    }
    if (keyEquals(key, "interval")) {
      if ((fields & 16) != 0 || !parser.parseUnsigned(value) || value > UINT8_MAX) return false;
      scratch->interval = static_cast<uint8_t>(value);
      fields |= 16;
      return true;
    }
    if (keyEquals(key, "first_poll_after")) {
      if ((fields & 32) != 0 || !parser.parseUnsigned(value) || value > UINT8_MAX) return false;
      scratch->firstPollAfter = static_cast<uint8_t>(value);
      fields |= 32;
      return true;
    }
    return parser.skipValue();
  });
  if (result != JsonParseResult::Success) return result;
  if (fields != 0x3f || protocol != PAIRING_PROTOCOL_VERSION ||
      !isBase64Url256(scratch->deviceCode, std::strlen(scratch->deviceCode)) ||
      !isCanonicalUserCode(scratch->userCode, std::strlen(scratch->userCode)) || scratch->expiresIn != 600 ||
      scratch->interval != 10 || scratch->firstPollAfter != 5) {
    return JsonParseResult::InvalidSchema;
  }
  response = scratch.value;
  return JsonParseResult::Success;
}

JsonParseResult parsePairingPollResponse(const char* json, const std::size_t length, PairingPollResponse& response) {
  PairingPollResponse parsed{};
  char status[9]{};
  uint8_t fields = 0;
  uint32_t protocol = 0;
  const JsonParseResult result = parseObject(json, length, 256, [&](JsonCursor& parser, const char* key) {
    if (keyEquals(key, "protocol")) {
      if ((fields & 1) != 0 || !parser.parseUnsigned(protocol)) return false;
      fields |= 1;
      return true;
    }
    if (keyEquals(key, "status")) {
      if ((fields & 2) != 0 || !parser.parseString(status, sizeof(status))) return false;
      fields |= 2;
      return true;
    }
    if (keyEquals(key, "account_hint")) {
      if ((fields & 4) != 0 || !parser.parseString(parsed.accountHint, sizeof(parsed.accountHint))) return false;
      fields |= 4;
      return true;
    }
    return parser.skipValue();
  });
  if (result != JsonParseResult::Success) return result;
  if ((fields & 3) != 3 || protocol != PAIRING_PROTOCOL_VERSION) return JsonParseResult::InvalidSchema;
  if (std::strcmp(status, "pending") == 0) {
    if ((fields & 4) != 0) return JsonParseResult::InvalidSchema;
    parsed.status = PollStatus::Pending;
  } else if (std::strcmp(status, "claimed") == 0) {
    if (!validUtf8(parsed.accountHint)) return JsonParseResult::InvalidSchema;
    parsed.status = PollStatus::Claimed;
  } else {
    return JsonParseResult::InvalidSchema;
  }
  response = parsed;
  return JsonParseResult::Success;
}

JsonParseResult parsePairingFinalizeResponse(const char* json, const std::size_t length,
                                             PairingFinalizeResponse& response) {
  SecureScratch<PairingFinalizeResponse> scratch;
  char tokenType[7]{};
  char scope[24]{};
  uint8_t fields = 0;
  uint32_t protocol = 0;
  const JsonParseResult result = parseObject(json, length, 512, [&](JsonCursor& parser, const char* key) {
    if (keyEquals(key, "protocol")) {
      if ((fields & 1) != 0 || !parser.parseUnsigned(protocol)) return false;
      fields |= 1;
      return true;
    }
    if (keyEquals(key, "access_token")) {
      if ((fields & 2) != 0 || !parser.parseString(scratch->accessToken, sizeof(scratch->accessToken))) return false;
      fields |= 2;
      return true;
    }
    if (keyEquals(key, "token_type")) {
      if ((fields & 4) != 0 || !parser.parseString(tokenType, sizeof(tokenType))) return false;
      fields |= 4;
      return true;
    }
    if (keyEquals(key, "device_id")) {
      if ((fields & 8) != 0 || !parser.parseString(scratch->deviceId, sizeof(scratch->deviceId))) return false;
      fields |= 8;
      return true;
    }
    if (keyEquals(key, "scope")) {
      if ((fields & 16) != 0 || !parser.parseString(scope, sizeof(scope))) return false;
      fields |= 16;
      return true;
    }
    return parser.skipValue();
  });
  if (result != JsonParseResult::Success) return result;
  if (fields != 0x1f || protocol != PAIRING_PROTOCOL_VERSION || std::strcmp(tokenType, "Bearer") != 0 ||
      std::strcmp(scope, POCKET_SCOPE) != 0 ||
      !isBase64Url256(scratch->accessToken, std::strlen(scratch->accessToken)) ||
      !validUuidText(scratch->deviceId)) {
    return JsonParseResult::InvalidSchema;
  }
  response = scratch.value;
  return JsonParseResult::Success;
}

JsonParseResult parsePocketSelfResponse(const char* json, const std::size_t length, PocketSelfResponse& response) {
  PocketSelfResponse parsed{};
  char scope[24]{};
  char status[8]{};
  uint8_t fields = 0;
  uint32_t protocol = 0;
  const JsonParseResult result = parseObject(json, length, 256, [&](JsonCursor& parser, const char* key) {
    if (keyEquals(key, "protocol")) {
      if ((fields & 1) != 0 || !parser.parseUnsigned(protocol)) return false;
      fields |= 1;
      return true;
    }
    if (keyEquals(key, "device_id")) {
      if ((fields & 2) != 0 || !parser.parseString(parsed.deviceId, sizeof(parsed.deviceId))) return false;
      fields |= 2;
      return true;
    }
    if (keyEquals(key, "name")) {
      if ((fields & 4) != 0 || !parser.parseString(parsed.name, sizeof(parsed.name))) return false;
      fields |= 4;
      return true;
    }
    if (keyEquals(key, "scope")) {
      if ((fields & 8) != 0 || !parser.parseString(scope, sizeof(scope))) return false;
      fields |= 8;
      return true;
    }
    if (keyEquals(key, "status")) {
      if ((fields & 16) != 0 || !parser.parseString(status, sizeof(status))) return false;
      fields |= 16;
      return true;
    }
    return parser.skipValue();
  });
  if (result != JsonParseResult::Success) return result;
  if (fields != 0x1f || protocol != PAIRING_PROTOCOL_VERSION || !validUuidText(parsed.deviceId) ||
      parsed.name[0] == '\0' || !validUtf8(parsed.name) || std::strcmp(scope, POCKET_SCOPE) != 0 ||
      std::strcmp(status, "active") != 0) {
    return JsonParseResult::InvalidSchema;
  }
  response = parsed;
  return JsonParseResult::Success;
}

JsonParseResult parsePocketStatusResponse(const char* json, const std::size_t length, const char* expectedStatus) {
  char status[17]{};
  uint8_t fields = 0;
  uint32_t protocol = 0;
  const JsonParseResult result = parseObject(json, length, 128, [&](JsonCursor& parser, const char* key) {
    if (keyEquals(key, "protocol")) {
      if ((fields & 1) != 0 || !parser.parseUnsigned(protocol)) return false;
      fields |= 1;
      return true;
    }
    if (keyEquals(key, "status")) {
      if ((fields & 2) != 0 || !parser.parseString(status, sizeof(status))) return false;
      fields |= 2;
      return true;
    }
    return parser.skipValue();
  });
  if (result != JsonParseResult::Success) return result;
  return fields == 3 && protocol == PAIRING_PROTOCOL_VERSION && expectedStatus != nullptr &&
                 std::strcmp(status, expectedStatus) == 0
             ? JsonParseResult::Success
             : JsonParseResult::InvalidSchema;
}

JsonParseResult parsePocketError(const char* json, const std::size_t length, char* error, const std::size_t errorSize) {
  uint8_t fields = 0;
  const JsonParseResult result = parseObject(json, length, 128, [&](JsonCursor& parser, const char* key) {
    if (keyEquals(key, "error")) {
      if ((fields & 1) != 0 || !parser.parseString(error, errorSize)) return false;
      fields |= 1;
      return true;
    }
    return parser.skipValue();
  });
  if (result != JsonParseResult::Success) return result;
  return fields == 1 && error != nullptr && error[0] != '\0' ? JsonParseResult::Success
                                                             : JsonParseResult::InvalidSchema;
}

void PairingMachine::resetUnpaired() {
  currentState = PairingState::Unpaired;
  unpairReturnState = PairingState::Paired;
  failures = 0;
  retryDelayActive = false;
}

void PairingMachine::restorePaired(const bool revokedTombstone) {
  currentState = revokedTombstone ? PairingState::RevokedNotice : PairingState::PairedUnverified;
  unpairReturnState = PairingState::Paired;
}

void PairingMachine::beginPairing() { currentState = PairingState::WifiCheck; }
void PairingMachine::wifiReady() { currentState = PairingState::Starting; }

void PairingMachine::startSucceeded(const PairingStartResponse& response, const uint32_t now) {
  startedAt = now;
  lastPollAt = now;
  ttlMs = static_cast<uint32_t>(response.expiresIn) * 1000U;
  pollIntervalMs = static_cast<uint32_t>(response.interval) * 1000U;
  firstPollDelayMs = static_cast<uint32_t>(response.firstPollAfter) * 1000U;
  failures = 0;
  firstPollPending = true;
  retryDelayActive = false;
  currentState = PairingState::CodeAndPolling;
}

void PairingMachine::startFailed() { currentState = PairingState::ErrorRetry; }

bool PairingMachine::expired(const uint32_t now) const {
  return (currentState == PairingState::CodeAndPolling || currentState == PairingState::ConfirmAccount ||
          currentState == PairingState::ErrorRetry) &&
         static_cast<uint32_t>(now - startedAt) >= ttlMs;
}

bool PairingMachine::pollDue(const uint32_t now) const {
  if (currentState != PairingState::CodeAndPolling || expired(now)) return false;
  if (retryDelayActive) return static_cast<uint32_t>(now - retryDelayStartedAt) >= retryDelayMs;
  const uint32_t delay = firstPollPending ? firstPollDelayMs : pollIntervalMs;
  return static_cast<uint32_t>(now - lastPollAt) >= delay;
}

uint16_t PairingMachine::secondsRemaining(const uint32_t now) const {
  const uint32_t elapsed = static_cast<uint32_t>(now - startedAt);
  if (elapsed >= ttlMs) return 0;
  return static_cast<uint16_t>((ttlMs - elapsed + 999U) / 1000U);
}

void PairingMachine::pollStarted(const uint32_t now) {
  lastPollAt = now;
  firstPollPending = false;
  retryDelayActive = false;
}

void PairingMachine::pollPending() { failures = 0; }

void PairingMachine::pollClaimed() {
  failures = 0;
  currentState = PairingState::ConfirmAccount;
}

void PairingMachine::pollRateLimited(const uint8_t retryAfterSeconds, const uint32_t now) {
  const uint8_t clamped = retryAfterSeconds < 5 ? 5 : (retryAfterSeconds > 30 ? 30 : retryAfterSeconds);
  retryDelayMs = static_cast<uint32_t>(clamped) * 1000U;
  retryDelayStartedAt = now;
  retryDelayActive = true;
}

void PairingMachine::pollTransportFailed(const uint32_t now) {
  if (failures < UINT8_MAX) ++failures;
  lastPollAt = now;
  retryDelayActive = false;
  if (failures >= 3) currentState = PairingState::ErrorRetry;
}

void PairingMachine::retryPolling(const uint32_t now) {
  if (expired(now)) {
    currentState = PairingState::Expired;
    return;
  }
  failures = 0;
  firstPollPending = false;
  retryDelayActive = false;
  lastPollAt = now - pollIntervalMs;
  currentState = PairingState::CodeAndPolling;
}

void PairingMachine::expire() { currentState = PairingState::Expired; }

void PairingMachine::confirmAccount() { currentState = PairingState::Finalizing; }
void PairingMachine::rejectAccount() { currentState = PairingState::Rejecting; }
void PairingMachine::rejectFinished() { resetUnpaired(); }
void PairingMachine::finalizeSucceeded() { currentState = PairingState::SavingNvs; }
void PairingMachine::finalizeConsumed() { currentState = PairingState::RestartRequired; }
void PairingMachine::finalizeFailed(const bool isExpired) {
  currentState = isExpired ? PairingState::Expired : PairingState::ErrorRetry;
}
void PairingMachine::credentialSaved(const bool success) {
  currentState = success ? PairingState::Verifying : PairingState::StorageError;
}
void PairingMachine::storageFailed() { currentState = PairingState::StorageError; }
void PairingMachine::verifySucceeded() { currentState = PairingState::Paired; }
void PairingMachine::verificationStarted() { currentState = PairingState::Verifying; }
void PairingMachine::verifyNetworkFailed() { currentState = PairingState::PairedUnverified; }
void PairingMachine::verifyRevoked() { currentState = PairingState::RevokedNotice; }
void PairingMachine::beginUnpair() {
  unpairReturnState = currentState == PairingState::PairedUnverified ? PairingState::PairedUnverified
                                                                    : PairingState::Paired;
  currentState = PairingState::UnpairConfirm;
}
void PairingMachine::confirmUnpair() {
  unpairReturnState = PairingState::Paired;
  currentState = PairingState::Unpairing;
}
void PairingMachine::cancelUnpair() {
  currentState = unpairReturnState;
  unpairReturnState = PairingState::Paired;
}
void PairingMachine::unpairSucceeded() { resetUnpaired(); }
void PairingMachine::unpairFailed() {
  unpairReturnState = PairingState::Paired;
  currentState = PairingState::ErrorRetry;
}
void PairingMachine::cancellationStarted() { currentState = PairingState::Cancelling; }
void PairingMachine::cancellationFinished() { currentState = PairingState::Cancelled; }

RevocationPersistenceState RevocationPersistenceCoordinator::recordInitial(const bool tombstoneSaved,
                                                                           const bool clearSucceeded) {
  current = tombstoneSaved ? RevocationPersistenceState::TombstoneSaved
                           : clearSucceeded ? RevocationPersistenceState::KeyCleared
                                            : RevocationPersistenceState::ClearPending;
  return current;
}

RevocationPersistenceState RevocationPersistenceCoordinator::recordClearRetry(const bool clearSucceeded) {
  if (current == RevocationPersistenceState::ClearPending && clearSucceeded) {
    current = RevocationPersistenceState::KeyCleared;
  }
  return current;
}

const char* pairingStateName(const PairingState state) {
  switch (state) {
    case PairingState::Unpaired:
      return "unpaired";
    case PairingState::WifiCheck:
      return "wifi_check";
    case PairingState::Starting:
      return "starting";
    case PairingState::CodeAndPolling:
      return "code_and_polling";
    case PairingState::ConfirmAccount:
      return "confirm_account";
    case PairingState::Rejecting:
      return "rejecting";
    case PairingState::Finalizing:
      return "finalizing";
    case PairingState::SavingNvs:
      return "saving_nvs";
    case PairingState::Verifying:
      return "verifying";
    case PairingState::PairedUnverified:
      return "paired_unverified";
    case PairingState::Paired:
      return "paired";
    case PairingState::UnpairConfirm:
      return "unpair_confirm";
    case PairingState::Unpairing:
      return "unpairing";
    case PairingState::Cancelling:
      return "cancelling";
    case PairingState::Cancelled:
      return "cancelled";
    case PairingState::Expired:
      return "expired";
    case PairingState::ErrorRetry:
      return "error_retry";
    case PairingState::RestartRequired:
      return "restart_required";
    case PairingState::StorageError:
      return "storage_error";
    case PairingState::RevokedNotice:
      return "revoked_notice";
  }
  return "unknown";
}

}  // namespace pocket
