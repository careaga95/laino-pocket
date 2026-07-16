#pragma once

#include <I18nKeys.h>

#include <cstddef>

namespace pocket {

struct Card {
  StrId label;
  StrId title;
  StrId subtitle;
  const StrId* lines;
  size_t lineCount;
};

inline constexpr size_t CARD_COUNT = 3;

const Card& cardAt(size_t index);

}  // namespace pocket
