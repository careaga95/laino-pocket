#include "PocketActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PocketActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PocketActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::POCKET);
  }
}

void PocketActivity::drawCard(const Rect rect) const {
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int padding = metrics.contentSidePadding;
  const int textWidth = std::max(0, rect.width - padding * 2);
  const int titleHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int subtitleHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int bulletHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int gap = std::max(2, metrics.verticalSpacing);
  const char* const bulletItems[] = {tr(STR_POCKET_REVIEW_PREMIUM), tr(STR_POCKET_CHECK_ENGINEERING),
                                     tr(STR_POCKET_AGREE_STRATEGY)};

  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  int textY = rect.y + padding;
  const auto title =
      renderer.truncatedText(UI_12_FONT_ID, tr(STR_POCKET_PREPARE_RENEWAL), textWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, rect.x + padding, textY, title.c_str(), true, EpdFontFamily::BOLD);

  textY += titleHeight + gap;
  const auto subtitle = renderer.truncatedText(SMALL_FONT_ID, tr(STR_POCKET_TODAY_TIME), textWidth);
  renderer.drawText(SMALL_FONT_ID, rect.x + padding, textY, subtitle.c_str());

  textY += subtitleHeight + gap * 2;
  for (size_t i = 0; i < std::size(bulletItems) && textY + bulletHeight <= rect.y + rect.height - padding; ++i) {
    const auto bullet = renderer.truncatedText(UI_10_FONT_ID, bulletItems[i], textWidth);
    renderer.drawText(UI_10_FONT_ID, rect.x + padding, textY, bullet.c_str());
    textY += bulletHeight + gap;
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
  GUI.drawHeader(renderer, Rect{safeLeft, headerY, safeRight - safeLeft, metrics.headerHeight}, tr(STR_LAINO_POCKET));

  const int cardX = safeLeft + metrics.contentSidePadding;
  const int cardY = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int cardWidth = safeRight - safeLeft - metrics.contentSidePadding * 2;
  const int cardHeight = safeBottom - cardY - metrics.verticalSpacing;
  drawCard(Rect{cardX, cardY, cardWidth, cardHeight});

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
