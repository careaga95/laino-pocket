#pragma once

#include "activities/Activity.h"

struct Rect;

class PocketActivity final : public Activity {
  void drawCard(Rect rect) const;

 public:
  explicit PocketActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pocket", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
