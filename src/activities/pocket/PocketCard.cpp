#include "PocketCard.h"

#include <cstring>

namespace pocket {
namespace {

template <size_t Capacity, size_t SourceSize>
void copyLiteral(char (&destination)[Capacity], const char (&source)[SourceSize]) {
  static_assert(SourceSize <= Capacity, "Fallback text exceeds Pocket card capacity");
  std::memcpy(destination, source, SourceSize);
}

}  // namespace

void loadFallbackCardBundle(CardBundle& destination) {
  destination = CardBundle{};
  destination.cardCount = 1;

  Card& card = destination.cards[0];
  copyLiteral(card.label, "Laino Next");
  copyLiteral(card.title, "Prepare renewal call");
  copyLiteral(card.subtitle, "Today \xC2\xB7 11:00");
  copyLiteral(card.lines[0], "Review premium figures");
  card.lineCount = 1;
}

}  // namespace pocket
