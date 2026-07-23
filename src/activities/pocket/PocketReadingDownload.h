#pragma once

#include <HalStorage.h>
#include <mbedtls/sha256.h>

#include "PocketPairingClient.h"
#include "PocketReading.h"

namespace pocket {

enum class ReadingDownloadResult : uint8_t {
  Pending,
  Success,
  SdUnavailable,
  OpenFailure,
  WriteFailure,
  LengthMismatch,
  HashMismatch,
  CommitFailure,
};

class PocketReadingFileSink final : public GatewayBodySink {
  const ReadingItem expected;
  HalFile file;
  mbedtls_sha256_context sha{};
  uint32_t received = 0;
  ReadingDownloadResult resultValue = ReadingDownloadResult::Pending;
  bool hashInitialized = false;
  bool fileOpen = false;
  char finalPath[96]{};
  char partPath[112]{};

 public:
  explicit PocketReadingFileSink(const ReadingItem& item);
  ~PocketReadingFileSink() override;

  bool begin(uint32_t contentLength) override;
  bool write(const uint8_t* data, size_t length) override;
  bool finish() override;
  void abort() override;

  [[nodiscard]] ReadingDownloadResult result() const { return resultValue; }
  [[nodiscard]] const char* path() const { return finalPath; }
};

bool isReadingDownloaded(const ReadingItem& item);
const char* readingDownloadResultName(ReadingDownloadResult result);

}  // namespace pocket
