#include "PocketCredentialStore.h"

namespace pocket {

CredentialLoadResult loadCredentialFromStorage(CredentialBlobStorage& storage, Credential& credential) {
  secureClear(&credential, sizeof(credential));
  std::size_t length = 0;
  const BlobStorageResult lengthResult = storage.length(length);
  if (lengthResult == BlobStorageResult::NotFound) return CredentialLoadResult::Absent;
  if (lengthResult != BlobStorageResult::Ok) return CredentialLoadResult::StorageError;
  if (length != CREDENTIAL_BLOB_BYTES) {
    return storage.remove() != BlobStorageResult::Error ? CredentialLoadResult::CorruptRemoved
                                                       : CredentialLoadResult::StorageError;
  }

  uint8_t blob[CREDENTIAL_BLOB_BYTES]{};
  if (storage.read(blob, sizeof(blob)) != BlobStorageResult::Ok) {
    secureClear(blob, sizeof(blob));
    return CredentialLoadResult::StorageError;
  }
  if (!decodeCredentialBlob(blob, sizeof(blob), credential)) {
    secureClear(blob, sizeof(blob));
    return storage.remove() != BlobStorageResult::Error ? CredentialLoadResult::CorruptRemoved
                                                       : CredentialLoadResult::StorageError;
  }
  secureClear(blob, sizeof(blob));
  return credential.state == CredentialState::Paired ? CredentialLoadResult::Paired
                                                     : CredentialLoadResult::RevokedTombstone;
}

bool saveCredentialToStorage(CredentialBlobStorage& storage, const Credential& credential) {
  uint8_t blob[CREDENTIAL_BLOB_BYTES]{};
  if (!encodeCredentialBlob(credential, blob, sizeof(blob))) {
    secureClear(blob, sizeof(blob));
    return false;
  }
  const bool saved = storage.write(blob, sizeof(blob)) == BlobStorageResult::Ok;
  secureClear(blob, sizeof(blob));
  return saved;
}

bool clearCredentialStorage(CredentialBlobStorage& storage) {
  std::size_t length = 0;
  const BlobStorageResult result = storage.length(length);
  if (result == BlobStorageResult::NotFound) return true;
  if (result != BlobStorageResult::Ok) return false;
  return storage.remove() != BlobStorageResult::Error;
}

}  // namespace pocket
