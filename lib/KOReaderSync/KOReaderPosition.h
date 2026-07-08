#pragma once

#include <string>

// A KOReader-protocol reading position: an XPath-like locator into the book's
// DOM plus a whole-book percentage. The percentage is the robust cross-device
// mechanism; the xpath adds paragraph precision for readers that honor it.
struct SavedProgressPosition {
  std::string xpath;  // e.g. "/body/DocFragment[8]/body/div[1]/p[42]/text().17"
  float percentage;   // 0.0 to 1.0 across the whole book
};
