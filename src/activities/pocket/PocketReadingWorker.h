#pragma once

#include <atomic>
#include <cstdint>

#include "PocketCredential.h"
#include "PocketPairingClient.h"
#include "PocketPairingWorker.h"
#include "PocketReading.h"
#include "PocketReadingDownload.h"

namespace pocket {

enum class ReadingWorkerOperation : uint8_t { Manifest, Download };

struct ReadingWorkerContext {
  WorkerLifecycle lifecycle;
  std::atomic<bool> cancelled{false};
  std::atomic<uint8_t> references{1};

  ReadingWorkerOperation operation = ReadingWorkerOperation::Manifest;
  Credential credential{};
  ReadingItem item{};
  PocketClientOutcome outcome{};
  ReadingDownloadResult downloadResult = ReadingDownloadResult::Pending;
  char json[MAX_READING_MANIFEST_JSON_BYTES + 1]{};
  char downloadedPath[96]{};
  uint16_t jsonLength = 0;
  uint32_t stackMargin = 0;
  uint32_t freeHeapBefore = 0;
  uint32_t freeHeapAfter = 0;
  uint32_t minimumFreeHeap = 0;

  void addReference();
  void releaseReference();

 private:
  ~ReadingWorkerContext();
};

}  // namespace pocket
