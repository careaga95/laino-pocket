#pragma once

#include <atomic>
#include <cstdint>

#include "PocketCard.h"
#include "PocketCredential.h"
#include "PocketPairingClient.h"
#include "PocketPairingWorker.h"

namespace pocket {

struct BundleWorkerContext {
  WorkerLifecycle lifecycle;
  std::atomic<bool> cancelled{false};
  std::atomic<uint8_t> references{1};

  Credential credential{};
  PocketClientOutcome outcome{};
  char json[MAX_SNAPSHOT_JSON_BYTES + 1]{};
  uint16_t jsonLength = 0;
  uint32_t stackMargin = 0;
  uint32_t freeHeapBefore = 0;
  uint32_t freeHeapAfter = 0;
  uint32_t minimumFreeHeap = 0;

  void addReference();
  void releaseReference();

 private:
  ~BundleWorkerContext();
};

}  // namespace pocket
