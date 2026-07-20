#pragma once

#include <cstddef>
#include <cstdint>

namespace pocket {

inline constexpr std::size_t CREDENTIAL_BLOB_BYTES = 56;
inline constexpr std::size_t DEVICE_UUID_BYTES = 16;
inline constexpr std::size_t DEVICE_TOKEN_BYTES = 32;
inline constexpr std::size_t DEVICE_TOKEN_TEXT_BYTES = 43;

enum class CredentialState : uint8_t { Paired = 1, RevokedTombstone = 2 };

struct Credential {
  CredentialState state = CredentialState::Paired;
  uint8_t deviceUuid[DEVICE_UUID_BYTES]{};
  uint8_t token[DEVICE_TOKEN_BYTES]{};
  uint8_t revocationReason = 0;
};

uint16_t credentialCrc16(const uint8_t* data, std::size_t length);
bool encodeCredentialBlob(const Credential& credential, uint8_t* output, std::size_t outputSize);
bool decodeCredentialBlob(const uint8_t* blob, std::size_t blobSize, Credential& credential);
bool parseUuid(const char* text, std::size_t length, uint8_t* output, std::size_t outputSize);
void formatUuidPrefix(const uint8_t* uuid, char* output, std::size_t outputSize);
bool decodeBase64UrlToken(const char* text, std::size_t length, uint8_t* output, std::size_t outputSize);
bool encodeBase64UrlToken(const uint8_t* token, std::size_t tokenSize, char* output, std::size_t outputSize);
void secureClear(void* memory, std::size_t length);

}  // namespace pocket
