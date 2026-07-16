#pragma once

#include "PocketCard.h"

class GfxRenderer;
struct Rect;

namespace pocket {

int preferredCardHeight(const GfxRenderer& renderer, const Card& card);
void drawCard(const GfxRenderer& renderer, Rect rect, const Card& card, size_t selectedIndex, size_t cardCount);

}  // namespace pocket
