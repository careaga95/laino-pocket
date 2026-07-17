#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

#include "PocketCredentialStore.h"
#include "PocketPairingClient.h"
#include "PocketPairingProtocol.h"
#include "PocketPairingWorker.h"
#include "activities/Activity.h"

class PocketPairingActivity final : public Activity {
 public:
  explicit PocketPairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PocketPairing", renderer, mappedInput) {}
  ~PocketPairingActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return workerRunning; }
  bool preventAutoSleep() override;

 private:
  enum class RetryOperation : uint8_t { None, Start, Poll, Finalize, DeleteSelf };
  enum class StorageOperation : uint8_t {
    None,
    LoadCredential,
    SaveCredential,
    ClearCredential,
    ClearRevokedCredential
  };
  enum class UnpairNotice : uint8_t { None, RemoteConfirmed, TokenInvalidOrRevoked, LocalOnly };

  pocket::CredentialStore credentialStore;
  pocket::Credential credential{};
  pocket::PairingMachine machine;
  pocket::RevocationPersistenceCoordinator revocationPersistence;
  pocket::PairingStartResponse startResponse{};
  pocket::PairingPollResponse pollResponse{};
  pocket::PairingFinalizeResponse finalizeResponse{};
  pocket::PocketSelfResponse selfResponse{};
  pocket::PocketClientOutcome workerOutcome{};

  TaskHandle_t workerHandle = nullptr;
  pocket::PairingWorkerContext* workerContext = nullptr;
  pocket::WorkerOperation workerOperation = pocket::WorkerOperation::None;
  pocket::WorkerOperation pendingAfterWifi = pocket::WorkerOperation::None;
  RetryOperation retryOperation = RetryOperation::None;
  StorageOperation storageOperation = StorageOperation::None;
  bool workerRunning = false;
  bool exitAfterCancellation = false;
  bool finalizeConfirm = true;
  bool credentialPresent = false;
  bool offerLocalOnlyUnpair = false;
  UnpairNotice unpairNotice = UnpairNotice::None;
  uint16_t lastRenderedSeconds = UINT16_MAX;
  uint32_t workerStackMargin = 0;

  static void workerTrampoline(void* context);
  static const char* workerOperationName(pocket::WorkerOperation operation);
  static void runWorker(pocket::PairingWorkerContext* context);
  bool startWorker(pocket::WorkerOperation operation);
  void handleWorkerLaunchFailure(pocket::WorkerOperation operation);
  void processWorkerResult();
  void requestOperationWithWifi(pocket::WorkerOperation operation);
  void onWifiSelectionComplete(bool connected);
  void beginNewPairing();
  void saveFinalizedCredential();
  void handleRevocation();
  void clearPairingSecrets();
  void clearCredentialRam();
  void handleBack();
  void handleConfirm();
  void retryLastOperation();
  void drawStateBody(int midY);
  void drawHints();
};
