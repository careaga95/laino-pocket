#include "PocketReadingDownload.h"

#include <cstring>

namespace pocket {
namespace {

bool expectedDigest(const char* hex, uint8_t (&output)[32]) {
  if (hex == nullptr || std::strlen(hex) != READING_SHA256_HEX_BYTES) return false;
  for (size_t index = 0; index < sizeof(output); ++index) {
    const auto nibble = [](const char value) -> int {
      if (value >= '0' && value <= '9') return value - '0';
      if (value >= 'a' && value <= 'f') return value - 'a' + 10;
      return -1;
    };
    const int high = nibble(hex[index * 2]);
    const int low = nibble(hex[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right, const size_t length) {
  uint8_t difference = 0;
  for (size_t index = 0; index < length; ++index) difference |= left[index] ^ right[index];
  return difference == 0;
}

}  // namespace

PocketReadingFileSink::PocketReadingFileSink(const ReadingItem& item) : expected(item) {
  readingPath(expected, finalPath, sizeof(finalPath));
  readingPartPath(expected, partPath, sizeof(partPath));
}

PocketReadingFileSink::~PocketReadingFileSink() {
  if (fileOpen || hashInitialized) abort();
}

bool PocketReadingFileSink::begin(const uint32_t contentLength) {
  if (contentLength != expected.bytes) {
    resultValue = ReadingDownloadResult::LengthMismatch;
    return false;
  }
  if (!Storage.ready()) {
    resultValue = ReadingDownloadResult::SdUnavailable;
    return false;
  }
  if (!Storage.exists(READING_DIRECTORY) && !Storage.mkdir(READING_DIRECTORY, true)) {
    resultValue = ReadingDownloadResult::OpenFailure;
    return false;
  }
  if (partPath[0] == '\0' || finalPath[0] == '\0') {
    resultValue = ReadingDownloadResult::OpenFailure;
    return false;
  }
  if (Storage.exists(partPath)) Storage.remove(partPath);
  if (!Storage.openFileForWrite("PKR", partPath, file)) {
    resultValue = ReadingDownloadResult::OpenFailure;
    return false;
  }
  fileOpen = true;
  mbedtls_sha256_init(&sha);
  if (mbedtls_sha256_starts(&sha, 0) != 0) {
    resultValue = ReadingDownloadResult::WriteFailure;
    abort();
    return false;
  }
  hashInitialized = true;
  received = 0;
  resultValue = ReadingDownloadResult::Pending;
  return true;
}

bool PocketReadingFileSink::write(const uint8_t* data, const size_t length) {
  if (!fileOpen || !hashInitialized || data == nullptr || length == 0 ||
      static_cast<uint64_t>(received) + length > expected.bytes) {
    resultValue = ReadingDownloadResult::LengthMismatch;
    return false;
  }
  if (file.write(data, length) != length || mbedtls_sha256_update(&sha, data, length) != 0) {
    resultValue = ReadingDownloadResult::WriteFailure;
    return false;
  }
  received += static_cast<uint32_t>(length);
  return true;
}

bool PocketReadingFileSink::finish() {
  if (!fileOpen || !hashInitialized || received != expected.bytes) {
    resultValue = ReadingDownloadResult::LengthMismatch;
    return false;
  }
  uint8_t actual[32]{};
  uint8_t expectedBytes[32]{};
  const bool hashFinished = mbedtls_sha256_finish(&sha, actual) == 0;
  mbedtls_sha256_free(&sha);
  hashInitialized = false;
  file.flush();
  const bool closed = file.close();
  fileOpen = false;
  if (!closed) {
    resultValue = ReadingDownloadResult::WriteFailure;
    Storage.remove(partPath);
    return false;
  }
  if (!hashFinished || !expectedDigest(expected.sha256, expectedBytes) ||
      !constantTimeEqual(actual, expectedBytes, sizeof(actual))) {
    resultValue = ReadingDownloadResult::HashMismatch;
    Storage.remove(partPath);
    return false;
  }
  if ((Storage.exists(finalPath) && !Storage.remove(finalPath)) || !Storage.rename(partPath, finalPath)) {
    resultValue = ReadingDownloadResult::CommitFailure;
    Storage.remove(partPath);
    return false;
  }
  resultValue = ReadingDownloadResult::Success;
  return true;
}

void PocketReadingFileSink::abort() {
  if (hashInitialized) {
    mbedtls_sha256_free(&sha);
    hashInitialized = false;
  }
  if (fileOpen) {
    file.close();
    fileOpen = false;
  }
  if (partPath[0] != '\0' && Storage.ready() && Storage.exists(partPath)) Storage.remove(partPath);
}

bool isReadingDownloaded(const ReadingItem& item) {
  char path[96];
  if (!readingPath(item, path, sizeof(path)) || !Storage.ready() || !Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("PKR", path, file)) return false;
  const bool exact = file.fileSize64() == item.bytes;
  const bool closed = file.close();
  return exact && closed;
}

const char* readingDownloadResultName(const ReadingDownloadResult result) {
  switch (result) {
    case ReadingDownloadResult::Pending:
      return "pending";
    case ReadingDownloadResult::Success:
      return "success";
    case ReadingDownloadResult::SdUnavailable:
      return "sd_unavailable";
    case ReadingDownloadResult::OpenFailure:
      return "open_failure";
    case ReadingDownloadResult::WriteFailure:
      return "write_failure";
    case ReadingDownloadResult::LengthMismatch:
      return "length_mismatch";
    case ReadingDownloadResult::HashMismatch:
      return "hash_mismatch";
    case ReadingDownloadResult::CommitFailure:
      return "commit_failure";
  }
  return "unknown";
}

}  // namespace pocket
