#include "PocketCredential.h"

#include <cstring>

namespace pocket {
namespace {

constexpr uint16_t CREDENTIAL_MAGIC = 0x4c50;
constexpr uint8_t CREDENTIAL_SCHEMA = 1;

int hexValue(const char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

int base64UrlValue(const char value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '-') return 62;
  if (value == '_') return 63;
  return -1;
}

char base64UrlCharacter(const uint8_t value) {
  constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  return ALPHABET[value & 0x3f];
}

bool tokenIsZero(const uint8_t* token) {
  uint8_t combined = 0;
  for (std::size_t i = 0; i < DEVICE_TOKEN_BYTES; ++i) combined |= token[i];
  return combined == 0;
}

}  // namespace

uint16_t credentialCrc16(const uint8_t* data, const std::size_t length) {
  uint16_t crc = 0xffff;
  for (std::size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) != 0 ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

bool encodeCredentialBlob(const Credential& credential, uint8_t* output, const std::size_t outputSize) {
  if (output != nullptr && outputSize == CREDENTIAL_BLOB_BYTES) secureClear(output, outputSize);
  if (output == nullptr || outputSize != CREDENTIAL_BLOB_BYTES ||
      (credential.state != CredentialState::Paired && credential.state != CredentialState::RevokedTombstone)) {
    return false;
  }
  if ((credential.state == CredentialState::Paired && tokenIsZero(credential.token)) ||
      (credential.state == CredentialState::RevokedTombstone && !tokenIsZero(credential.token))) {
    return false;
  }

  std::memset(output, 0, outputSize);
  output[0] = static_cast<uint8_t>(CREDENTIAL_MAGIC >> 8);
  output[1] = static_cast<uint8_t>(CREDENTIAL_MAGIC & 0xff);
  output[2] = CREDENTIAL_SCHEMA;
  output[3] = static_cast<uint8_t>(credential.state);
  std::memcpy(output + 4, credential.deviceUuid, DEVICE_UUID_BYTES);
  std::memcpy(output + 20, credential.token, DEVICE_TOKEN_BYTES);
  output[52] = credential.revocationReason;
  output[53] = 0;
  const uint16_t crc = credentialCrc16(output, 54);
  output[54] = static_cast<uint8_t>(crc >> 8);
  output[55] = static_cast<uint8_t>(crc & 0xff);
  return true;
}

bool decodeCredentialBlob(const uint8_t* blob, const std::size_t blobSize, Credential& credential) {
  secureClear(&credential, sizeof(credential));
  if (blob == nullptr || blobSize != CREDENTIAL_BLOB_BYTES || blob[0] != static_cast<uint8_t>(CREDENTIAL_MAGIC >> 8) ||
      blob[1] != static_cast<uint8_t>(CREDENTIAL_MAGIC & 0xff) || blob[2] != CREDENTIAL_SCHEMA || blob[53] != 0) {
    return false;
  }
  const uint16_t storedCrc = static_cast<uint16_t>((static_cast<uint16_t>(blob[54]) << 8) | blob[55]);
  if (credentialCrc16(blob, 54) != storedCrc) return false;

  const auto state = static_cast<CredentialState>(blob[3]);
  if (state != CredentialState::Paired && state != CredentialState::RevokedTombstone) return false;
  const bool zeroToken = tokenIsZero(blob + 20);
  if ((state == CredentialState::Paired && zeroToken) || (state == CredentialState::RevokedTombstone && !zeroToken)) {
    return false;
  }

  Credential decoded{};
  decoded.state = state;
  std::memcpy(decoded.deviceUuid, blob + 4, DEVICE_UUID_BYTES);
  std::memcpy(decoded.token, blob + 20, DEVICE_TOKEN_BYTES);
  decoded.revocationReason = blob[52];
  credential = decoded;
  secureClear(&decoded, sizeof(decoded));
  return true;
}

bool parseUuid(const char* text, const std::size_t length, uint8_t* output, const std::size_t outputSize) {
  if (text == nullptr || output == nullptr || length != 36 || outputSize != DEVICE_UUID_BYTES) return false;
  constexpr std::size_t HYPHENS[] = {8, 13, 18, 23};
  std::size_t outputIndex = 0;
  int highNibble = -1;
  for (std::size_t i = 0; i < length; ++i) {
    bool hyphenPosition = false;
    for (const std::size_t position : HYPHENS) hyphenPosition = hyphenPosition || i == position;
    if (hyphenPosition) {
      if (text[i] != '-') return false;
      continue;
    }
    const int nibble = hexValue(text[i]);
    if (nibble < 0) return false;
    if (highNibble < 0) {
      highNibble = nibble;
    } else {
      if (outputIndex >= outputSize) return false;
      output[outputIndex++] = static_cast<uint8_t>((highNibble << 4) | nibble);
      highNibble = -1;
    }
  }
  return outputIndex == outputSize && highNibble < 0;
}

void formatUuidPrefix(const uint8_t* uuid, char* output, const std::size_t outputSize) {
  constexpr char HEX[] = "0123456789abcdef";
  if (output == nullptr || outputSize == 0) return;
  if (uuid == nullptr || outputSize < 9) {
    output[0] = '\0';
    return;
  }
  for (std::size_t i = 0; i < 4; ++i) {
    output[i * 2] = HEX[uuid[i] >> 4];
    output[i * 2 + 1] = HEX[uuid[i] & 0x0f];
  }
  output[8] = '\0';
}

bool decodeBase64UrlToken(const char* text, const std::size_t length, uint8_t* output, const std::size_t outputSize) {
  if (output != nullptr && outputSize == DEVICE_TOKEN_BYTES) secureClear(output, outputSize);
  if (text == nullptr || output == nullptr || length != DEVICE_TOKEN_TEXT_BYTES || outputSize != DEVICE_TOKEN_BYTES) {
    return false;
  }
  uint32_t accumulator = 0;
  uint8_t bits = 0;
  std::size_t outputIndex = 0;
  for (std::size_t i = 0; i < length; ++i) {
    const int value = base64UrlValue(text[i]);
    if (value < 0) {
      secureClear(output, outputSize);
      return false;
    }
    accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
    bits = static_cast<uint8_t>(bits + 6);
    if (bits >= 8) {
      bits = static_cast<uint8_t>(bits - 8);
      if (outputIndex >= outputSize) {
        secureClear(output, outputSize);
        return false;
      }
      output[outputIndex++] = static_cast<uint8_t>((accumulator >> bits) & 0xff);
    }
  }
  // 32 bytes encode to 43 characters with two unused low bits; reject non-canonical aliases.
  const bool valid = outputIndex == outputSize && bits == 2 && (accumulator & 0x03U) == 0;
  if (!valid) secureClear(output, outputSize);
  return valid;
}

bool encodeBase64UrlToken(const uint8_t* token, const std::size_t tokenSize, char* output,
                          const std::size_t outputSize) {
  if (token == nullptr || output == nullptr || tokenSize != DEVICE_TOKEN_BYTES ||
      outputSize < DEVICE_TOKEN_TEXT_BYTES + 1) {
    return false;
  }
  uint32_t accumulator = 0;
  uint8_t bits = 0;
  std::size_t outputIndex = 0;
  for (std::size_t i = 0; i < tokenSize; ++i) {
    accumulator = (accumulator << 8) | token[i];
    bits = static_cast<uint8_t>(bits + 8);
    while (bits >= 6) {
      bits = static_cast<uint8_t>(bits - 6);
      output[outputIndex++] = base64UrlCharacter(static_cast<uint8_t>(accumulator >> bits));
    }
  }
  if (bits > 0) output[outputIndex++] = base64UrlCharacter(static_cast<uint8_t>(accumulator << (6 - bits)));
  if (outputIndex != DEVICE_TOKEN_TEXT_BYTES) return false;
  output[outputIndex] = '\0';
  return true;
}

void secureClear(void* memory, const std::size_t length) {
  volatile uint8_t* cursor = static_cast<volatile uint8_t*>(memory);
  for (std::size_t i = 0; i < length; ++i) cursor[i] = 0;
}

}  // namespace pocket
