#pragma once

#include "PocketCredential.h"

namespace pocket {

enum class CredentialLoadResult : uint8_t { Absent, Paired, RevokedTombstone, CorruptRemoved, StorageError };
enum class BlobStorageResult : uint8_t { Ok, NotFound, Error };

class CredentialBlobStorage {
 public:
  virtual ~CredentialBlobStorage() = default;
  virtual BlobStorageResult length(std::size_t& value) = 0;
  virtual BlobStorageResult read(uint8_t* output, std::size_t outputSize) = 0;
  virtual BlobStorageResult write(const uint8_t* data, std::size_t dataSize) = 0;
  virtual BlobStorageResult remove() = 0;
};

CredentialLoadResult loadCredentialFromStorage(CredentialBlobStorage& storage, Credential& credential);
bool saveCredentialToStorage(CredentialBlobStorage& storage, const Credential& credential);
bool clearCredentialStorage(CredentialBlobStorage& storage);

class CredentialStore {
 public:
  CredentialLoadResult load(Credential& credential);
  bool save(const Credential& credential);
  bool saveRevokedTombstone(const Credential& credential, uint8_t reason);
  bool clear();
};

}  // namespace pocket
