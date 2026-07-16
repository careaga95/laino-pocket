#include "PocketCardRenderer.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "PocketText.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace pocket {
namespace {

constexpr size_t textBufferSize = 128;
constexpr size_t positionBufferSize = 24;

void drawTruncatedText(const GfxRenderer& renderer, const int fontId, const int x, const int y, const char* text,
                       const int maxWidth, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  char buffer[textBufferSize];
  const char* fitted = truncateToWidth(
      text, maxWidth, buffer, [&](const char* candidate) { return renderer.getTextWidth(fontId, candidate, style); });
  if (fitted[0] != '\0') {
    renderer.drawText(fontId, x, y, fitted, true, style);
  }
}

bool hasVerticalSpace(const int y, const int lineHeight, const int bottom) {
  return lineHeight > 0 && y >= 0 && y + lineHeight <= bottom;
}

}  // namespace

int preferredCardHeight(const GfxRenderer& renderer, const Card& card) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int padding = std::max(0, metrics.contentSidePadding);
  const int gap = std::max(2, metrics.verticalSpacing);
  int height = padding * 2 + renderer.getLineHeight(SMALL_FONT_ID);

  if (card.title[0] != '\0') {
    height += gap + renderer.getLineHeight(UI_12_FONT_ID);
  }
  if (card.subtitle[0] != '\0') {
    height += gap + renderer.getLineHeight(SMALL_FONT_ID);
  }
  if (card.lineCount > 0) {
    height += gap * 2;
    height += static_cast<int>(card.lineCount) * renderer.getLineHeight(UI_10_FONT_ID);
    height += static_cast<int>(card.lineCount - 1) * gap;
  }
  return std::max(0, height);
}

void drawCard(const GfxRenderer& renderer, const Rect rect, const Card& card, const size_t selectedIndex,
              const size_t cardCount) {
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }

  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int padding = std::max(0, metrics.contentSidePadding);
  const int gap = std::max(2, metrics.verticalSpacing);
  const int textX = rect.x + padding;
  const int textWidth = std::max(0, rect.width - padding * 2);
  const int contentBottom = rect.y + rect.height - padding;
  if (textWidth <= 0 || contentBottom <= rect.y) {
    return;
  }

  int textY = rect.y + padding;
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  if (hasVerticalSpace(textY, labelHeight, contentBottom)) {
    char position[positionBufferSize];
    position[0] = '\0';
    if (cardCount > 0) {
      std::snprintf(position, sizeof(position), I18N.get(StrId::STR_POCKET_POSITION_FORMAT),
                    static_cast<unsigned>(selectedIndex + 1), static_cast<unsigned>(cardCount));
    }

    const int positionWidth = renderer.getTextWidth(SMALL_FONT_ID, position);
    const int labelWidth = std::max(0, textWidth - positionWidth - gap);
    drawTruncatedText(renderer, SMALL_FONT_ID, textX, textY, card.label, labelWidth);
    if (position[0] != '\0' && positionWidth <= textWidth) {
      renderer.drawText(SMALL_FONT_ID, textX + textWidth - positionWidth, textY, position);
    }
    textY += labelHeight + gap;
  }

  const char* title = card.title;
  const int titleHeight = renderer.getLineHeight(UI_12_FONT_ID);
  if (title[0] != '\0' && hasVerticalSpace(textY, titleHeight, contentBottom)) {
    drawTruncatedText(renderer, UI_12_FONT_ID, textX, textY, title, textWidth, EpdFontFamily::BOLD);
    textY += titleHeight + gap;
  }

  const char* subtitle = card.subtitle;
  const int subtitleHeight = renderer.getLineHeight(SMALL_FONT_ID);
  if (subtitle[0] != '\0' && hasVerticalSpace(textY, subtitleHeight, contentBottom)) {
    drawTruncatedText(renderer, SMALL_FONT_ID, textX, textY, subtitle, textWidth);
    textY += subtitleHeight + gap * 2;
  }

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  for (size_t i = 0; i < card.lineCount && hasVerticalSpace(textY, lineHeight, contentBottom); ++i) {
    drawTruncatedText(renderer, UI_10_FONT_ID, textX, textY, card.lines[i], textWidth);
    textY += lineHeight + gap;
  }
}

}  // namespace pocket
