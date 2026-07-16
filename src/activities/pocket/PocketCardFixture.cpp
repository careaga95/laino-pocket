#include "PocketCardFixture.h"

#include "PocketCard.h"

namespace pocket {

const char COMPILED_CARD_JSON[] = R"json({
  "protocolVersion": 1,
  "cards": [
    {
      "label": "Laino Next",
      "title": "Prepare renewal call",
      "subtitle": "Today · 11:00",
      "lines": [
        "- Review premium figures",
        "- Check engineering note",
        "- Agree market strategy"
      ]
    },
    {
      "label": "Daily Command",
      "title": "Today",
      "subtitle": "Wednesday · 3 priorities",
      "lines": [
        "- Review Pocket Phase 3",
        "- Follow up with engineering",
        "- Prepare client meeting"
      ]
    },
    {
      "label": "Waiting On",
      "title": "Waiting on",
      "subtitle": "2 open items",
      "lines": [
        "- Engineering note · Quantum",
        "- Updated terms · Market"
      ]
    }
  ]
})json";

const size_t COMPILED_CARD_JSON_LENGTH = sizeof(COMPILED_CARD_JSON) - 1;

static_assert(COMPILED_CARD_JSON_LENGTH <= MAX_JSON_DOCUMENT_BYTES);

}  // namespace pocket
