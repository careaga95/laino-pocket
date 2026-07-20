#pragma once

#include <atomic>
#include <cstdint>

#include "PocketCredential.h"
#include "PocketPairingClient.h"

namespace pocket {

enum class WorkerOperation : uint8_t { None, Start, Poll, Finalize, Reject, Verify, DeleteSelf };
enum class WorkerPhase : uint8_t { Idle, Running, Cancelling, Released, StartFailed };

class WorkerLifecycle {
 public:
  bool begin();
  void launchFailed();
  void requestCancel();
  void release();
  [[nodiscard]] WorkerPhase phase() const { return current.load(std::memory_order_acquire); }
  [[nodiscard]] bool ownerMayFinish() const;

 private:
  std::atomic<WorkerPhase> current{WorkerPhase::Idle};
};

enum class UnpairDisposition : uint8_t { RemoteConfirmed, TokenInvalidOrRevoked, OfferLocalOnly };
UnpairDisposition classifyUnpairResult(PocketClientResult result);

struct PairingWorkerContext {
  WorkerLifecycle lifecycle;
  WorkerOperation operation = WorkerOperation::None;
  bool finalizeConfirm = true;
  std::atomic<bool> cancelled{false};
  std::atomic<uint8_t> references{1};

  Credential credential{};
  PairingStartResponse startResponse{};
  PairingPollResponse pollResponse{};
  PairingFinalizeResponse finalizeResponse{};
  PocketSelfResponse selfResponse{};
  PocketClientOutcome outcome{};
  uint32_t stackMargin = 0;

  void addReference();
  void releaseReference();
  void clearSecrets();

 private:
  ~PairingWorkerContext();
};

}  // namespace pocket
