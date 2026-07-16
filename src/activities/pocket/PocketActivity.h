#pragma once

#include "PocketBundleCache.h"
#include "PocketCard.h"
#include "PocketCardSelection.h"
#include "PocketSdCacheStorage.h"
#include "activities/Activity.h"

struct Rect;

class PocketActivity final : public Activity {
  pocket::PocketSdCacheStorage cacheStorage;
  pocket::PocketBundleCache cache{cacheStorage};
  pocket::CardBundle cardBundle;
  pocket::CardSelection cardSelection{0};

 public:
  explicit PocketActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pocket", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
