#include "PocketCredentialStore.h"

#include <nvs.h>

#include <cstring>

namespace pocket {
namespace {

constexpr char NVS_NAMESPACE[] = "laino_pocket";
constexpr char NVS_KEY[] = "credential_v1";

class NvsBlobStorage final : public CredentialBlobStorage {
 public:
  explicit NvsBlobStorage(const nvs_handle_t handle) : handle(handle) {}
  BlobStorageResult length(std::size_t& value) override {
    value = 0;
    const esp_err_t error = nvs_get_blob(handle, NVS_KEY, nullptr, &value);
    if (error == ESP_ERR_NVS_NOT_FOUND) return BlobStorageResult::NotFound;
    return error == ESP_OK ? BlobStorageResult::Ok : BlobStorageResult::Error;
  }
  BlobStorageResult read(uint8_t* output, const std::size_t outputSize) override {
    std::size_t actual = outputSize;
    const esp_err_t error = nvs_get_blob(handle, NVS_KEY, output, &actual);
    return error == ESP_OK && actual == outputSize ? BlobStorageResult::Ok : BlobStorageResult::Error;
  }
  BlobStorageResult write(const uint8_t* data, const std::size_t dataSize) override {
    if (nvs_set_blob(handle, NVS_KEY, data, dataSize) != ESP_OK) return BlobStorageResult::Error;
    return nvs_commit(handle) == ESP_OK ? BlobStorageResult::Ok : BlobStorageResult::Error;
  }
  BlobStorageResult remove() override {
    const esp_err_t error = nvs_erase_key(handle, NVS_KEY);
    if (error == ESP_ERR_NVS_NOT_FOUND) return BlobStorageResult::NotFound;
    if (error != ESP_OK) return BlobStorageResult::Error;
    return nvs_commit(handle) == ESP_OK ? BlobStorageResult::Ok : BlobStorageResult::Error;
  }

 private:
  nvs_handle_t handle;
};

}  // namespace

CredentialLoadResult CredentialStore::load(Credential& credential) {
  nvs_handle_t handle = 0;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return CredentialLoadResult::StorageError;
  NvsBlobStorage storage(handle);
  const CredentialLoadResult result = loadCredentialFromStorage(storage, credential);
  nvs_close(handle);
  return result;
}

bool CredentialStore::save(const Credential& credential) {
  nvs_handle_t handle = 0;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
  NvsBlobStorage storage(handle);
  const bool saved = saveCredentialToStorage(storage, credential);
  nvs_close(handle);
  return saved;
}

bool CredentialStore::saveRevokedTombstone(const Credential& credential, const uint8_t reason) {
  Credential tombstone{};
  tombstone.state = CredentialState::RevokedTombstone;
  std::memcpy(tombstone.deviceUuid, credential.deviceUuid, sizeof(tombstone.deviceUuid));
  tombstone.revocationReason = reason;
  const bool saved = save(tombstone);
  secureClear(&tombstone, sizeof(tombstone));
  return saved;
}

bool CredentialStore::clear() {
  nvs_handle_t handle = 0;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
  NvsBlobStorage storage(handle);
  const bool cleared = clearCredentialStorage(storage);
  nvs_close(handle);
  return cleared;
}

}  // namespace pocket
