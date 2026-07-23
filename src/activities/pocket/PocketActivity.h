#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "PocketBundleWorker.h"
#include "PocketCredentialStore.h"
#include "PocketSdCacheStorage.h"
#include "PocketSnapshot.h"
#include "PocketSnapshotCache.h"
#include "activities/Activity.h"

struct Rect;

class PocketActivity final : public Activity {
  enum class SyncNotice : uint8_t {
    None,
    Updating,
    Updated,
    NoWifi,
    LowMemory,
    ServiceUnavailable,
    InvalidData,
    CacheFailure,
    PairingNeedsAttention
  };
  enum class View : uint8_t { Today, Sections, Section, Detail };
  enum class TodaySlot : uint8_t { NextMeeting, TopPriority, Waiting, Owe, Count };

  pocket::PocketSdCacheStorage cacheStorage;
  pocket::PocketSnapshotCache cache{cacheStorage};
  pocket::PocketSnapshot snapshot;
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
  bool autoSyncPending = false;
  bool initialRenderComplete = false;
  bool detailFromToday = false;
  bool sectionFromToday = false;
  uint32_t workerStackMargin = 0;
  View view = View::Today;
  size_t todaySelection = 0;
  size_t sectionIndex = 0;
  size_t itemIndex = 0;

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
  void restoreCachedSnapshot();
  [[nodiscard]] bool canSync() const { return credentialPresent && !credentialNeedsAttention; }
  [[nodiscard]] bool refreshDue() const;
  [[nodiscard]] const char* headerLabel() const;
  [[nodiscard]] const char* sectionLabel(const pocket::SnapshotSection& section) const;
  [[nodiscard]] const pocket::SnapshotSection* selectedSection() const;
  [[nodiscard]] const pocket::SnapshotItem* selectedItem() const;
  [[nodiscard]] pocket::SnapshotSectionId todaySectionIdAt(size_t index) const;
  [[nodiscard]] const pocket::SnapshotSection* sectionForId(pocket::SnapshotSectionId id) const;
  [[nodiscard]] const pocket::SnapshotItem* todayItemAt(size_t index) const;
  [[nodiscard]] const pocket::SnapshotItem* selectedTodayItem() const;
  [[nodiscard]] size_t todayItemCount() const;
  [[nodiscard]] const char* todaySlotLabel(size_t index) const;
  [[nodiscard]] const char* todayEmptyLabel(size_t index) const;
  [[nodiscard]] bool canOpenTodaySelection() const;
  bool selectSection(pocket::SnapshotSectionId id);
  bool selectItem(const pocket::SnapshotItem* item);
  void moveSelection(int delta);
  void openSelection();
  void openSections();
  void goBack();
  void goToday();
  void renderToday(const Rect& content);
  void renderSections(const Rect& content);
  void renderSection(const Rect& content);
  void renderDetail(const Rect& content);
  void formatFreshness(char* buffer, size_t capacity) const;
};
