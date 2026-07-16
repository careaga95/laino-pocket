#pragma once

#include "PocketCard.h"
#include "PocketCardSelection.h"
#include "activities/Activity.h"

struct Rect;

class PocketActivity final : public Activity {
  pocket::CardSelection cardSelection{pocket::CARD_COUNT};

 public:
  explicit PocketActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pocket", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
