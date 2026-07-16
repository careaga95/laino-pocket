#include "PocketActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "PocketCard.h"
#include "PocketCardRenderer.h"
#include "components/UITheme.h"

void PocketActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PocketActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::POCKET);
    return;
  }

  bool changed = false;
  if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
    RenderLock lock;
    changed = cardSelection.selectPrevious();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
    RenderLock lock;
    changed = cardSelection.selectNext();
  }

  if (changed) {
    requestUpdate();
  }
}

void PocketActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  int viewableTop = 0;
  int viewableRight = 0;
  int viewableBottom = 0;
  int viewableLeft = 0;
  renderer.getOrientedViewableTRBL(&viewableTop, &viewableRight, &viewableBottom, &viewableLeft);

  const Rect hintSafeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int safeLeft = std::max(viewableLeft, hintSafeArea.x);
  const int safeTop = std::max(viewableTop, hintSafeArea.y);
  const int safeRight = std::min(screenWidth - viewableRight, hintSafeArea.x + hintSafeArea.width);
  const int safeBottom = std::min(screenHeight - viewableBottom, hintSafeArea.y + hintSafeArea.height);

  const int headerY = std::max(safeTop, metrics.topPadding);
  const int headerWidth = std::max(0, safeRight - safeLeft);
  if (headerWidth > 0 && metrics.headerHeight > 0) {
    GUI.drawHeader(renderer, Rect{safeLeft, headerY, headerWidth, metrics.headerHeight}, tr(STR_LAINO_POCKET));
  }

  // getScreenSafeArea() currently reserves front hints only, so Pocket keeps side-hint clearance local.
  const int sideInset = std::max(metrics.contentSidePadding, metrics.sideButtonHintsWidth + metrics.verticalSpacing);
  const int cardX = safeLeft + sideInset;
  const int cardY = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int cardWidth = std::max(0, safeRight - safeLeft - sideInset * 2);
  const int availableCardHeight = std::max(0, safeBottom - cardY - metrics.verticalSpacing);
  const auto& card = pocket::cardAt(cardSelection.index());
  const int cardHeight = std::min(availableCardHeight, pocket::preferredCardHeight(renderer, card));
  pocket::drawCard(renderer, Rect{cardX, cardY, cardWidth, cardHeight}, card, cardSelection.index(),
                   pocket::CARD_COUNT);

  const char* previousLabel = cardSelection.canSelectPrevious() ? tr(STR_POCKET_PREVIOUS) : "";
  const char* nextLabel = cardSelection.canSelectNext() ? tr(STR_POCKET_NEXT) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", previousLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, previousLabel, nextLabel);

  renderer.displayBuffer();
}
