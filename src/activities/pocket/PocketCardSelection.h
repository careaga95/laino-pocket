#pragma once

#include <cstddef>

namespace pocket {

class CardSelection final {
  const size_t count;
  size_t selectedIndex = 0;

 public:
  explicit constexpr CardSelection(const size_t count) : count(count) {}

  [[nodiscard]] constexpr size_t index() const { return selectedIndex; }
  [[nodiscard]] constexpr bool canSelectPrevious() const { return selectedIndex > 0; }
  [[nodiscard]] constexpr bool canSelectNext() const { return count > 0 && selectedIndex + 1 < count; }

  constexpr bool selectPrevious() {
    if (!canSelectPrevious()) {
      return false;
    }
    --selectedIndex;
    return true;
  }

  constexpr bool selectNext() {
    if (!canSelectNext()) {
      return false;
    }
    ++selectedIndex;
    return true;
  }
};

}  // namespace pocket
