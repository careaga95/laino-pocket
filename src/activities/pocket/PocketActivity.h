#pragma once

#include "PocketCard.h"
#include "PocketCardSelection.h"
#include "activities/Activity.h"

struct Rect;

class PocketActivity final : public Activity {
  pocket::CardBundle cardBundle;
  pocket::CardSelection cardSelection{0};

 public:
  explicit PocketActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pocket", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
