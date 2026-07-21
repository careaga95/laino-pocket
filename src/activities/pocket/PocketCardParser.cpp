#include "PocketCardParser.h"

#include <cstring>

namespace pocket {
namespace {

constexpr size_t KEY_BUFFER_BYTES = 32;
static_assert(PROTOCOL_VERSION <= 9, "Pocket protocol parsing expects a single decimal digit");

bool isWhitespace(const char value) { return value == ' ' || value == '\t' || value == '\r' || value == '\n'; }

bool isDigit(const char value) { return value >= '0' && value <= '9'; }

int hexValue(const char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

class Utf8Validator final {
  uint32_t codepoint = 0;
  uint32_t minimum = 0;
  uint8_t remaining = 0;

 public:
  bool feed(const uint8_t byte) {
    if (remaining == 0) {
      if (byte <= 0x7F) return true;
      if (byte >= 0xC2 && byte <= 0xDF) {
        codepoint = byte & 0x1FU;
        minimum = 0x80;
        remaining = 1;
        return true;
      }
      if (byte >= 0xE0 && byte <= 0xEF) {
        codepoint = byte & 0x0FU;
        minimum = 0x800;
        remaining = 2;
        return true;
      }
      if (byte >= 0xF0 && byte <= 0xF4) {
        codepoint = byte & 0x07U;
        minimum = 0x10000;
        remaining = 3;
        return true;
      }
      return false;
    }

    if ((byte & 0xC0U) != 0x80U) return false;
    codepoint = (codepoint << 6U) | (byte & 0x3FU);
    --remaining;
    if (remaining != 0) return true;
    return codepoint >= minimum && codepoint <= 0x10FFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF);
  }

  [[nodiscard]] bool complete() const { return remaining == 0; }
};

class BoundedParser final {
  const char* const json;
  const size_t length;
  CardBundle* const output;
  const size_t maximumCards;
  size_t position = 0;

  void skipWhitespace() {
    while (position < length && isWhitespace(json[position])) ++position;
  }

  [[nodiscard]] char peek() const { return position < length ? json[position] : '\0'; }

  bool consume(const char expected) {
    if (position >= length || json[position] != expected) return false;
    ++position;
    return true;
  }

  ParseResult readHexQuad(uint16_t& value) {
    if (length - position < 4) return ParseResult::MalformedJson;
    value = 0;
    for (size_t i = 0; i < 4; ++i) {
      const int digit = hexValue(json[position++]);
      if (digit < 0) return ParseResult::MalformedJson;
      value = static_cast<uint16_t>((value << 4U) | static_cast<uint16_t>(digit));
    }
    return ParseResult::Success;
  }

  ParseResult parseString(char* destination, const size_t destinationCapacity, const size_t maximumBytes,
                          const bool enforceLimit, const ParseResult invalidUtf8Result, bool* truncated = nullptr) {
    if (!consume('"')) return ParseResult::MalformedJson;

    Utf8Validator utf8;
    size_t outputLength = 0;
    bool didTruncate = false;

    const auto emitByte = [&](const uint8_t byte) -> ParseResult {
      if (byte == 0 || !utf8.feed(byte)) return invalidUtf8Result;
      if (enforceLimit && outputLength >= maximumBytes) return ParseResult::TextTooLong;
      if (destination != nullptr && destinationCapacity > 0) {
        if (outputLength + 1 < destinationCapacity) {
          destination[outputLength] = static_cast<char>(byte);
        } else {
          didTruncate = true;
        }
      }
      ++outputLength;
      return ParseResult::Success;
    };

    const auto emitCodepoint = [&](const uint32_t codepoint) -> ParseResult {
      uint8_t bytes[4];
      size_t count = 0;
      if (codepoint <= 0x7F) {
        bytes[count++] = static_cast<uint8_t>(codepoint);
      } else if (codepoint <= 0x7FF) {
        bytes[count++] = static_cast<uint8_t>(0xC0U | (codepoint >> 6U));
        bytes[count++] = static_cast<uint8_t>(0x80U | (codepoint & 0x3FU));
      } else if (codepoint <= 0xFFFF) {
        bytes[count++] = static_cast<uint8_t>(0xE0U | (codepoint >> 12U));
        bytes[count++] = static_cast<uint8_t>(0x80U | ((codepoint >> 6U) & 0x3FU));
        bytes[count++] = static_cast<uint8_t>(0x80U | (codepoint & 0x3FU));
      } else {
        bytes[count++] = static_cast<uint8_t>(0xF0U | (codepoint >> 18U));
        bytes[count++] = static_cast<uint8_t>(0x80U | ((codepoint >> 12U) & 0x3FU));
        bytes[count++] = static_cast<uint8_t>(0x80U | ((codepoint >> 6U) & 0x3FU));
        bytes[count++] = static_cast<uint8_t>(0x80U | (codepoint & 0x3FU));
      }
      for (size_t i = 0; i < count; ++i) {
        const ParseResult result = emitByte(bytes[i]);
        if (result != ParseResult::Success) return result;
      }
      return ParseResult::Success;
    };

    while (position < length) {
      const uint8_t byte = static_cast<uint8_t>(json[position++]);
      if (byte == '"') {
        if (!utf8.complete()) return invalidUtf8Result;
        if (destination != nullptr && destinationCapacity > 0) {
          const size_t terminator = outputLength < destinationCapacity ? outputLength : destinationCapacity - 1;
          destination[terminator] = '\0';
        }
        if (truncated != nullptr) *truncated = didTruncate;
        return ParseResult::Success;
      }
      if (byte < 0x20U) return ParseResult::MalformedJson;
      if (byte != '\\') {
        const ParseResult result = emitByte(byte);
        if (result != ParseResult::Success) return result;
        continue;
      }

      if (position >= length) return ParseResult::MalformedJson;
      const char escaped = json[position++];
      uint8_t decoded = 0;
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          decoded = static_cast<uint8_t>(escaped);
          break;
        case 'b':
          decoded = '\b';
          break;
        case 'f':
          decoded = '\f';
          break;
        case 'n':
          decoded = '\n';
          break;
        case 'r':
          decoded = '\r';
          break;
        case 't':
          decoded = '\t';
          break;
        case 'u': {
          uint16_t first = 0;
          ParseResult result = readHexQuad(first);
          if (result != ParseResult::Success) return result;
          uint32_t codepoint = first;
          if (first >= 0xD800 && first <= 0xDBFF) {
            if (!consume('\\') || !consume('u')) return invalidUtf8Result;
            uint16_t second = 0;
            result = readHexQuad(second);
            if (result != ParseResult::Success) return result;
            if (second < 0xDC00 || second > 0xDFFF) return invalidUtf8Result;
            codepoint = 0x10000U + ((static_cast<uint32_t>(first) - 0xD800U) << 10U) +
                        (static_cast<uint32_t>(second) - 0xDC00U);
          } else if (first >= 0xDC00 && first <= 0xDFFF) {
            return invalidUtf8Result;
          }
          result = emitCodepoint(codepoint);
          if (result != ParseResult::Success) return result;
          continue;
        }
        default:
          return ParseResult::MalformedJson;
      }

      const ParseResult result = emitByte(decoded);
      if (result != ParseResult::Success) return result;
    }
    return ParseResult::MalformedJson;
  }

  ParseResult parseKey(char (&key)[KEY_BUFFER_BYTES], bool& truncated) {
    truncated = false;
    return parseString(key, sizeof(key), 0, false, ParseResult::MalformedJson, &truncated);
  }

  ParseResult parseNumber(size_t* start = nullptr, size_t* end = nullptr) {
    const size_t numberStart = position;
    if (consume('-') && position >= length) return ParseResult::MalformedJson;

    if (consume('0')) {
      if (position < length && isDigit(json[position])) return ParseResult::MalformedJson;
    } else {
      if (position >= length || json[position] < '1' || json[position] > '9') return ParseResult::MalformedJson;
      while (position < length && isDigit(json[position])) ++position;
    }

    if (consume('.')) {
      if (position >= length || !isDigit(json[position])) return ParseResult::MalformedJson;
      while (position < length && isDigit(json[position])) ++position;
    }

    if (position < length && (json[position] == 'e' || json[position] == 'E')) {
      ++position;
      if (position < length && (json[position] == '+' || json[position] == '-')) ++position;
      if (position >= length || !isDigit(json[position])) return ParseResult::MalformedJson;
      while (position < length && isDigit(json[position])) ++position;
    }

    if (start != nullptr) *start = numberStart;
    if (end != nullptr) *end = position;
    return ParseResult::Success;
  }

  ParseResult parseLiteral(const char* literal, const size_t literalLength) {
    if (length - position < literalLength || std::memcmp(json + position, literal, literalLength) != 0) {
      return ParseResult::MalformedJson;
    }
    position += literalLength;
    return ParseResult::Success;
  }

  ParseResult skipValue(const size_t parentDepth) {
    skipWhitespace();
    switch (peek()) {
      case '"':
        return parseString(nullptr, 0, 0, false, ParseResult::MalformedJson);
      case '{':
        return skipObject(parentDepth + 1);
      case '[':
        return skipArray(parentDepth + 1);
      case 't':
        return parseLiteral("true", 4);
      case 'f':
        return parseLiteral("false", 5);
      case 'n':
        return parseLiteral("null", 4);
      default:
        if (peek() == '-' || isDigit(peek())) return parseNumber();
        return ParseResult::MalformedJson;
    }
  }

  ParseResult skipObject(const size_t depth) {
    if (depth > MAX_JSON_NESTING) return ParseResult::MalformedJson;
    if (!consume('{')) return ParseResult::MalformedJson;
    skipWhitespace();
    if (consume('}')) return ParseResult::Success;

    while (true) {
      if (peek() != '"') return ParseResult::MalformedJson;
      ParseResult result = parseString(nullptr, 0, 0, false, ParseResult::MalformedJson);
      if (result != ParseResult::Success) return result;
      skipWhitespace();
      if (!consume(':')) return ParseResult::MalformedJson;
      result = skipValue(depth);
      if (result != ParseResult::Success) return result;
      skipWhitespace();
      if (consume('}')) return ParseResult::Success;
      if (!consume(',')) return ParseResult::MalformedJson;
      skipWhitespace();
    }
  }

  ParseResult skipArray(const size_t depth) {
    if (depth > MAX_JSON_NESTING) return ParseResult::MalformedJson;
    if (!consume('[')) return ParseResult::MalformedJson;
    skipWhitespace();
    if (consume(']')) return ParseResult::Success;

    while (true) {
      ParseResult result = skipValue(depth);
      if (result != ParseResult::Success) return result;
      skipWhitespace();
      if (consume(']')) return ParseResult::Success;
      if (!consume(',')) return ParseResult::MalformedJson;
      skipWhitespace();
    }
  }

  ParseResult parseRequiredText(char* destination, const size_t capacity) {
    skipWhitespace();
    if (peek() != '"') return ParseResult::WrongFieldType;
    return parseString(destination, capacity, capacity - 1, true, ParseResult::InvalidUtf8);
  }

  ParseResult parseLines(Card* card, const size_t depth) {
    skipWhitespace();
    if (!consume('[')) return ParseResult::WrongFieldType;
    if (depth > MAX_JSON_NESTING) return ParseResult::MalformedJson;
    skipWhitespace();

    size_t lineCount = 0;
    if (!consume(']')) {
      while (true) {
        if (lineCount >= MAX_LINES_PER_CARD) return ParseResult::TooManyLines;
        skipWhitespace();
        if (peek() != '"') return ParseResult::WrongFieldType;
        ParseResult result = parseString(card != nullptr ? card->lines[lineCount] : nullptr, MAX_LINE_BYTES + 1,
                                         MAX_LINE_BYTES, true, ParseResult::InvalidUtf8);
        if (result != ParseResult::Success) return result;
        ++lineCount;
        skipWhitespace();
        if (consume(']')) break;
        if (!consume(',')) return ParseResult::MalformedJson;
        skipWhitespace();
        if (peek() == ']') return ParseResult::MalformedJson;
      }
    }

    if (card != nullptr) card->lineCount = lineCount;
    return ParseResult::Success;
  }

  ParseResult parseCard(Card* card, const size_t depth) {
    if (depth > MAX_JSON_NESTING) return ParseResult::MalformedJson;
    if (!consume('{')) return ParseResult::WrongFieldType;
    skipWhitespace();

    constexpr uint8_t LABEL_SEEN = 1U << 0U;
    constexpr uint8_t TITLE_SEEN = 1U << 1U;
    constexpr uint8_t SUBTITLE_SEEN = 1U << 2U;
    constexpr uint8_t LINES_SEEN = 1U << 3U;
    constexpr uint8_t ALL_FIELDS = LABEL_SEEN | TITLE_SEEN | SUBTITLE_SEEN | LINES_SEEN;
    uint8_t seen = 0;

    if (!consume('}')) {
      while (true) {
        if (peek() != '"') return ParseResult::MalformedJson;
        char key[KEY_BUFFER_BYTES]{};
        bool keyTruncated = false;
        ParseResult result = parseKey(key, keyTruncated);
        if (result != ParseResult::Success) return result;
        skipWhitespace();
        if (!consume(':')) return ParseResult::MalformedJson;
        skipWhitespace();

        uint8_t field = 0;
        if (!keyTruncated && std::strcmp(key, "label") == 0) field = LABEL_SEEN;
        if (!keyTruncated && std::strcmp(key, "title") == 0) field = TITLE_SEEN;
        if (!keyTruncated && std::strcmp(key, "subtitle") == 0) field = SUBTITLE_SEEN;
        if (!keyTruncated && std::strcmp(key, "lines") == 0) field = LINES_SEEN;
        if (field != 0 && (seen & field) != 0) return ParseResult::DuplicateField;
        seen = static_cast<uint8_t>(seen | field);

        if (field == LABEL_SEEN) {
          result = parseRequiredText(card != nullptr ? card->label : nullptr, MAX_LABEL_BYTES + 1);
        } else if (field == TITLE_SEEN) {
          result = parseRequiredText(card != nullptr ? card->title : nullptr, MAX_TITLE_BYTES + 1);
        } else if (field == SUBTITLE_SEEN) {
          result = parseRequiredText(card != nullptr ? card->subtitle : nullptr, MAX_SUBTITLE_BYTES + 1);
        } else if (field == LINES_SEEN) {
          result = parseLines(card, depth + 1);
        } else {
          result = skipValue(depth);
        }
        if (result != ParseResult::Success) return result;

        skipWhitespace();
        if (consume('}')) break;
        if (!consume(',')) return ParseResult::MalformedJson;
        skipWhitespace();
      }
    }

    return seen == ALL_FIELDS ? ParseResult::Success : ParseResult::MissingRequiredField;
  }

  ParseResult parseCards(const size_t depth) {
    skipWhitespace();
    if (!consume('[')) return ParseResult::WrongFieldType;
    if (depth > MAX_JSON_NESTING) return ParseResult::MalformedJson;
    skipWhitespace();
    if (consume(']')) return ParseResult::EmptyCards;

    size_t cardCount = 0;
    while (true) {
      if (cardCount >= maximumCards) return ParseResult::TooManyCards;
      skipWhitespace();
      if (peek() != '{') return ParseResult::WrongFieldType;
      ParseResult result = parseCard(output != nullptr ? &output->cards[cardCount] : nullptr, depth + 1);
      if (result != ParseResult::Success) return result;
      ++cardCount;
      skipWhitespace();
      if (consume(']')) break;
      if (!consume(',')) return ParseResult::MalformedJson;
      skipWhitespace();
      if (peek() == ']') return ParseResult::MalformedJson;
    }

    if (output != nullptr) output->cardCount = cardCount;
    return cardCount >= MIN_CARDS ? ParseResult::Success : ParseResult::EmptyCards;
  }

  ParseResult parseProtocolVersion() {
    skipWhitespace();
    if (peek() != '-' && !isDigit(peek())) return ParseResult::WrongFieldType;
    size_t start = 0;
    size_t end = 0;
    const ParseResult result = parseNumber(&start, &end);
    if (result != ParseResult::Success) return result;
    return end - start == 1 && json[start] == static_cast<char>('0' + PROTOCOL_VERSION)
               ? ParseResult::Success
               : ParseResult::UnsupportedProtocolVersion;
  }

  ParseResult parseRootObject(const size_t depth) {
    if (depth > MAX_JSON_NESTING) return ParseResult::MalformedJson;
    if (!consume('{')) return ParseResult::WrongFieldType;
    skipWhitespace();

    constexpr uint8_t VERSION_SEEN = 1U << 0U;
    constexpr uint8_t CARDS_SEEN = 1U << 1U;
    constexpr uint8_t ALL_FIELDS = VERSION_SEEN | CARDS_SEEN;
    uint8_t seen = 0;

    if (!consume('}')) {
      while (true) {
        if (peek() != '"') return ParseResult::MalformedJson;
        char key[KEY_BUFFER_BYTES]{};
        bool keyTruncated = false;
        ParseResult result = parseKey(key, keyTruncated);
        if (result != ParseResult::Success) return result;
        skipWhitespace();
        if (!consume(':')) return ParseResult::MalformedJson;
        skipWhitespace();

        uint8_t field = 0;
        if (!keyTruncated && std::strcmp(key, "protocolVersion") == 0) field = VERSION_SEEN;
        if (!keyTruncated && std::strcmp(key, "cards") == 0) field = CARDS_SEEN;
        if (field != 0 && (seen & field) != 0) return ParseResult::DuplicateField;
        seen = static_cast<uint8_t>(seen | field);

        if (field == VERSION_SEEN) {
          result = parseProtocolVersion();
        } else if (field == CARDS_SEEN) {
          result = parseCards(depth + 1);
        } else {
          result = skipValue(depth);
        }
        if (result != ParseResult::Success) return result;

        skipWhitespace();
        if (consume('}')) break;
        if (!consume(',')) return ParseResult::MalformedJson;
        skipWhitespace();
      }
    }

    return seen == ALL_FIELDS ? ParseResult::Success : ParseResult::MissingRequiredField;
  }

 public:
  BoundedParser(const char* source, const size_t sourceLength, CardBundle* destination, const size_t cardLimit)
      : json(source), length(sourceLength), output(destination), maximumCards(cardLimit) {}

  ParseResult parseDocument() {
    skipWhitespace();
    const ParseResult result = parseRootObject(1);
    if (result != ParseResult::Success) return result;
    skipWhitespace();
    return position == length ? ParseResult::Success : ParseResult::MalformedJson;
  }
};

}  // namespace

ParseResult parseCardBundle(const char* json, const size_t jsonLength, CardBundle& destination) {
  const ParseResult validationResult = validateCardBundle(json, jsonLength);
  if (validationResult != ParseResult::Success) return validationResult;

  // Both passes use identical schema and limits. Publication is expected to be infallible after validation;
  // adding destination-dependent parsing behavior would violate this atomicity guarantee.
  destination = CardBundle{};
  BoundedParser publisher(json, jsonLength, &destination, MAX_CARDS);
  return publisher.parseDocument();
}

ParseResult validateCardBundle(const char* json, const size_t jsonLength, const size_t maximumCards) {
  if (json == nullptr || jsonLength == 0) return ParseResult::EmptyInput;
  if (jsonLength > MAX_JSON_DOCUMENT_BYTES) return ParseResult::DocumentTooLarge;
  if (maximumCards < MIN_CARDS || maximumCards > MAX_CARDS) return ParseResult::TooManyCards;

  BoundedParser validator(json, jsonLength, nullptr, maximumCards);
  return validator.parseDocument();
}

const char* parseResultName(const ParseResult result) {
  switch (result) {
    case ParseResult::Success:
      return "success";
    case ParseResult::EmptyInput:
      return "empty_input";
    case ParseResult::DocumentTooLarge:
      return "document_too_large";
    case ParseResult::MalformedJson:
      return "malformed_json";
    case ParseResult::UnsupportedProtocolVersion:
      return "unsupported_protocol_version";
    case ParseResult::MissingRequiredField:
      return "missing_required_field";
    case ParseResult::WrongFieldType:
      return "wrong_field_type";
    case ParseResult::DuplicateField:
      return "duplicate_field";
    case ParseResult::EmptyCards:
      return "empty_cards";
    case ParseResult::TooManyCards:
      return "too_many_cards";
    case ParseResult::TooManyLines:
      return "too_many_lines";
    case ParseResult::TextTooLong:
      return "text_too_long";
    case ParseResult::InvalidUtf8:
      return "invalid_utf8";
  }
  return "unknown";
}

}  // namespace pocket
