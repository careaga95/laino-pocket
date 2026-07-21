#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "PocketBundleCache.h"
#include "PocketBundleWorker.h"
#include "PocketCard.h"
#include "PocketCardSelection.h"
#include "PocketCredentialStore.h"
#include "PocketSdCacheStorage.h"
#include "activities/Activity.h"

struct Rect;

class PocketActivity final : public Activity {
  enum class SyncNotice : uint8_t { None, Updating, Updated, Failed, PairingNeedsAttention };

  pocket::PocketSdCacheStorage cacheStorage;
  pocket::PocketBundleCache cache{cacheStorage};
  pocket::CardBundle cardBundle;
  pocket::CardSelection cardSelection{0};
  pocket::CredentialStore credentialStore;
  pocket::Credential credential{};
  pocket::BundleWorkerContext* workerContext = nullptr;
  TaskHandle_t workerHandle = nullptr;
  SyncNotice syncNotice = SyncNotice::None;
  bool credentialPresent = false;
  bool credentialNeedsAttention = false;
  bool workerRunning = false;
  bool pendingAfterWifi = false;
  bool exitAfterCancellation = false;
  bool longPressFired = false;
  uint32_t workerStackMargin = 0;

 public:
  explicit PocketActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pocket", renderer, mappedInput) {}
  ~PocketActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return workerRunning; }
  bool preventAutoSleep() override { return workerRunning; }

 private:
  static void workerTrampoline(void* context);
  static void runWorker(pocket::BundleWorkerContext* context);
  void reloadCredential();
  void openHari();
  void requestSync();
  void onWifiSelectionComplete(bool connected);
  bool startWorker();
  void processWorkerResult();
  void cancelWorkerAndExit();
  [[nodiscard]] bool canSync() const { return credentialPresent && !credentialNeedsAttention; }
  [[nodiscard]] const char* headerLabel() const;
};
