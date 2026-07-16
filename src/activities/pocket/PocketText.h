#pragma once

#include <Utf8.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pocket {

namespace detail {

inline size_t previousUtf8Boundary(const char* text, size_t length) {
  if (length == 0) {
    return 0;
  }

  size_t position = length - 1;
  while (position > 0 && (static_cast<uint8_t>(text[position]) & 0xC0U) == 0x80U) {
    --position;
  }
  return position;
}

}  // namespace detail

template <size_t Capacity, typename Measure>
const char* truncateToWidth(const char* text, const int maxWidth, char (&buffer)[Capacity], Measure&& measure) {
  static_assert(Capacity >= 4, "Pocket text buffer must fit an ellipsis and terminator");
  buffer[0] = '\0';
  if (text == nullptr || text[0] == '\0' || maxWidth <= 0) {
    return buffer;
  }

  if (measure(text) <= maxWidth) {
    return text;
  }

  constexpr char ellipsis[] = "\xE2\x80\xA6";
  constexpr size_t ellipsisBytes = sizeof(ellipsis) - 1;
  constexpr size_t maxContentBytes = Capacity - ellipsisBytes - 1;

  size_t contentLength = 0;
  while (contentLength < maxContentBytes && text[contentLength] != '\0') {
    ++contentLength;
  }
  contentLength = static_cast<size_t>(utf8SafeTruncateBuffer(text, static_cast<int>(contentLength)));

  while (true) {
    std::memcpy(buffer, text, contentLength);
    std::memcpy(buffer + contentLength, ellipsis, sizeof(ellipsis));
    if (measure(buffer) <= maxWidth) {
      return buffer;
    }
    if (contentLength == 0) {
      buffer[0] = '\0';
      return buffer;
    }
    contentLength = detail::previousUtf8Boundary(text, contentLength);
  }
}

}  // namespace pocket
