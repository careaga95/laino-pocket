#include "PocketCard.h"

#include <array>

namespace pocket {
namespace {

constexpr std::array<StrId, 3> lainoNextLines = {
    StrId::STR_POCKET_REVIEW_PREMIUM,
    StrId::STR_POCKET_CHECK_ENGINEERING,
    StrId::STR_POCKET_AGREE_STRATEGY,
};

constexpr std::array<StrId, 3> dailyCommandLines = {
    StrId::STR_POCKET_REVIEW_PHASE_TWO,
    StrId::STR_POCKET_FOLLOW_UP_ENGINEERING,
    StrId::STR_POCKET_PREPARE_CLIENT_MEETING,
};

constexpr std::array<StrId, 2> waitingOnLines = {
    StrId::STR_POCKET_ENGINEERING_QUANTUM,
    StrId::STR_POCKET_UPDATED_TERMS_MARKET,
};

constexpr std::array<Card, CARD_COUNT> cards = {{
    {StrId::STR_POCKET_LAINO_NEXT, StrId::STR_POCKET_PREPARE_RENEWAL, StrId::STR_POCKET_TODAY_TIME,
     lainoNextLines.data(), lainoNextLines.size()},
    {StrId::STR_POCKET_DAILY_COMMAND, StrId::STR_POCKET_TODAY, StrId::STR_POCKET_WEDNESDAY_PRIORITIES,
     dailyCommandLines.data(), dailyCommandLines.size()},
    {StrId::STR_POCKET_WAITING_ON_LABEL, StrId::STR_POCKET_WAITING_ON_TITLE, StrId::STR_POCKET_OPEN_ITEMS,
     waitingOnLines.data(), waitingOnLines.size()},
}};

}  // namespace

const Card& cardAt(const size_t index) { return cards[index < cards.size() ? index : 0]; }

}  // namespace pocket
