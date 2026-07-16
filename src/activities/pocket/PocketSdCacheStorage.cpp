#include "PocketSdCacheStorage.h"

#include <HalStorage.h>

#include <algorithm>

namespace pocket {
namespace {

bool readFully(HalFile& file, void* destination, const size_t length, size_t& actual) {
  actual = 0;
  auto* bytes = static_cast<uint8_t*>(destination);
  while (actual < length) {
    const int count = file.read(bytes + actual, length - actual);
    if (count <= 0) break;
    actual += static_cast<size_t>(count);
  }
  return actual == length;
}

}  // namespace

bool PocketSdCacheStorage::available() const { return Storage.ready(); }

bool PocketSdCacheStorage::ensureDirectory(const char* path) {
  return Storage.exists(path) || Storage.mkdir(path, true);
}

CacheResult PocketSdCacheStorage::read(const char* path, uint8_t* header, const size_t headerCapacity,
                                       size_t& headerRead, char* payload, const size_t payloadCapacity,
                                       size_t& payloadRead, size_t& fileSize) {
  headerRead = 0;
  payloadRead = 0;
  fileSize = 0;
  if (!available()) return CacheResult::SdUnavailable;
  if (!Storage.exists(path)) return CacheResult::FileNotFound;

  HalFile file;
  if (!Storage.openFileForRead("PKC", path, file)) return CacheResult::ReadFailure;
  fileSize = file.fileSize();
  const size_t wantedHeader = std::min(headerCapacity, fileSize);
  readFully(file, header, wantedHeader, headerRead);
  const size_t remaining = fileSize > headerRead ? fileSize - headerRead : 0;
  const size_t wantedPayload = std::min(payloadCapacity, remaining);
  readFully(file, payload, wantedPayload, payloadRead);
  const bool closed = file.close();
  return headerRead == wantedHeader && payloadRead == wantedPayload && closed ? CacheResult::Success
                                                                              : CacheResult::ReadFailure;
}

CacheResult PocketSdCacheStorage::write(const char* path, const uint8_t* header, const size_t headerLength,
                                        const char* payload, const size_t payloadLength) {
  if (!available()) return CacheResult::SdUnavailable;
  HalFile file;
  if (!Storage.openFileForWrite("PKC", path, file)) return CacheResult::WriteFailure;
  const bool headerWritten = file.write(header, headerLength) == headerLength;
  const bool payloadWritten = headerWritten && file.write(payload, payloadLength) == payloadLength;
  file.flush();
  const bool closed = file.close();
  return headerWritten && payloadWritten && closed ? CacheResult::Success : CacheResult::WriteFailure;
}

}  // namespace pocket
