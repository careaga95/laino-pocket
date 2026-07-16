#pragma once

#include "PocketHttpsProbe.h"
#include "activities/Activity.h"

class PocketHttpsProbeActivity final : public Activity {
 public:
  explicit PocketHttpsProbeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PocketHttpsProbe", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return presentation.isTesting(); }

 private:
  pocket::HttpsProbePresentation presentation;
};
