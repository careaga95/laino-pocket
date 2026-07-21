#pragma once

#include <cstddef>

namespace pocket {

inline constexpr unsigned PROTOCOL_VERSION = 1;
inline constexpr size_t MAX_JSON_DOCUMENT_BYTES = 4096;
inline constexpr size_t MIN_CARDS = 1;
inline constexpr size_t MAX_CARDS = 8;
inline constexpr size_t MAX_REMOTE_CARDS = 4;
static_assert(MAX_REMOTE_CARDS <= MAX_CARDS);
inline constexpr size_t MAX_LABEL_BYTES = 48;
inline constexpr size_t MAX_TITLE_BYTES = 64;
inline constexpr size_t MAX_SUBTITLE_BYTES = 80;
inline constexpr size_t MAX_LINES_PER_CARD = 6;
inline constexpr size_t MAX_LINE_BYTES = 96;
inline constexpr size_t MAX_JSON_NESTING = 16;

struct Card {
  char label[MAX_LABEL_BYTES + 1]{};
  char title[MAX_TITLE_BYTES + 1]{};
  char subtitle[MAX_SUBTITLE_BYTES + 1]{};
  char lines[MAX_LINES_PER_CARD][MAX_LINE_BYTES + 1]{};
  size_t lineCount = 0;
};

struct CardBundle {
  Card cards[MAX_CARDS]{};
  size_t cardCount = 0;

  [[nodiscard]] const Card& cardAt(size_t index) const {
    return cards[cardCount > 0 && index < cardCount && index < MAX_CARDS ? index : 0];
  }
};

// Replaces destination with a small known-good bundle without invoking the JSON parser.
void loadFallbackCardBundle(CardBundle& destination);

}  // namespace pocket
