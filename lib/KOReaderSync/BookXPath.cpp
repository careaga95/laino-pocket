#include "BookXPath.h"

#include <Logging.h>
#include <Memory.h>
#include <epub/XmlSax.h>

#include <cstdio>
#include <cstring>

namespace {

using freeink::book::Arena;
using freeink::book::BookStatus;
using freeink::book::XmlHandler;
using freeink::book::XmlSax;

// XmlSax parse working set: inflate window + parse chunk (+ expat's own
// bounded heap). One transient allocation per mapping call.
constexpr size_t kParseScratchSize = 64 * 1024;

constexpr int kMaxDepth = 24;        // element ancestry below <body>
constexpr int kMaxSiblingTags = 24;  // distinct child tag names tracked per level
constexpr int kMaxSteps = 16;        // parsed xpath ancestry steps

const char* localName(const char* qname) {
  const char* colon = strrchr(qname, ':');
  return colon != nullptr ? colon + 1 : qname;
}

bool isSuppressedElement(const char* local) {
  return strcmp(local, "head") == 0 || strcmp(local, "style") == 0 || strcmp(local, "script") == 0 ||
         strcmp(local, "title") == 0;
}

// ChapterLayout's block list — these flush the paragraph accumulator.
bool isBlockElement(const char* local) {
  static const char* kBlocks[] = {"p",  "h1",    "h2",      "h3",      "h4",     "h5",    "h6",         "blockquote",
                                  "li", "div",   "section", "article", "figure", "aside", "figcaption", "ul",
                                  "ol", "table", "tr",      "td",      "th",     "dt",    "dd"};
  for (const char* b : kBlocks) {
    if (strcmp(local, b) == 0) return true;
  }
  return false;
}

uint32_t fnvHash(const char* s) {
  uint32_t hash = 2166136261u;
  while (*s != '\0') {
    hash ^= static_cast<uint8_t>(*s++);
    hash *= 16777619u;
  }
  return hash;
}

struct AncestryLevel {
  char tag[16];
  int siblingIndex;  // 1-based, among same-tag siblings
  // Child tag occurrence counts (for the level BELOW this one).
  uint32_t childTagHashes[kMaxSiblingTags];
  int childTagCounts[kMaxSiblingTags];
  int childTagKinds;
};

struct XPathStep {
  char tag[16];
  int siblingIndex;
};

// Parses the element steps between "]/body/" and the terminal "/text()..."
// or ".NN" of a KOReader xpath. Returns the step count, 0 on failure.
int parseXPathSteps(const std::string& xpath, XPathStep* steps, int* charOffsetOut) {
  *charOffsetOut = 0;
  static const char kFrag[] = "/body/DocFragment[";
  const size_t fragPos = xpath.find(kFrag);
  if (fragPos == std::string::npos) return 0;
  const size_t closeBracket = xpath.find(']', fragPos + strlen(kFrag));
  if (closeBracket == std::string::npos) return 0;
  static const char kBody[] = "/body/";
  if (xpath.compare(closeBracket + 1, strlen(kBody), kBody) != 0) return 0;
  size_t pos = closeBracket + 1 + strlen(kBody);

  // Terminal: "/text()[K].N" or "/text().N" or ".N" directly on the element.
  size_t stepsEnd = xpath.rfind("/text()");
  if (stepsEnd == std::string::npos || stepsEnd < pos) {
    const size_t dot = xpath.rfind('.');
    stepsEnd = (dot != std::string::npos && dot > pos) ? dot : xpath.size();
  }
  const size_t dot = xpath.rfind('.');
  if (dot != std::string::npos && dot + 1 < xpath.size()) {
    int val = 0;
    bool numeric = true;
    for (size_t i = dot + 1; i < xpath.size(); ++i) {
      if (xpath[i] < '0' || xpath[i] > '9') {
        numeric = false;
        break;
      }
      val = val * 10 + (xpath[i] - '0');
    }
    if (numeric) *charOffsetOut = val;
  }
  if (stepsEnd <= pos) return 0;

  int count = 0;
  while (pos < stepsEnd && count < kMaxSteps) {
    const size_t slash = xpath.find('/', pos);
    const size_t segEnd = (slash != std::string::npos && slash < stepsEnd) ? slash : stepsEnd;
    XPathStep& step = steps[count];
    const size_t bracket = xpath.find('[', pos);
    const size_t nameEnd = (bracket != std::string::npos && bracket < segEnd) ? bracket : segEnd;
    const size_t nameLen = nameEnd - pos;
    if (nameLen == 0 || nameLen >= sizeof(step.tag)) return 0;
    memcpy(step.tag, xpath.c_str() + pos, nameLen);
    step.tag[nameLen] = '\0';
    step.siblingIndex = 1;
    if (bracket != std::string::npos && bracket < segEnd) {
      const size_t close = xpath.find(']', bracket + 1);
      if (close == std::string::npos || close > segEnd) return 0;
      int idx = 0;
      for (size_t i = bracket + 1; i < close; ++i) {
        if (xpath[i] < '0' || xpath[i] > '9') return 0;
        idx = idx * 10 + (xpath[i] - '0');
      }
      step.siblingIndex = idx > 0 ? idx : 1;
    }
    ++count;
    pos = (slash != std::string::npos && slash < stepsEnd) ? slash + 1 : stepsEnd;
  }
  return count;
}

// Streams the chapter, replicating ChapterLayout's extracted-text accounting
// (which defines charStart) while tracking DOM element ancestry with per-tag
// sibling indices. Runs in one of two modes:
//   Locate:  find the ancestry containing character offset `target`
//   Resolve: find the character offset where a given ancestry begins
class XPathScanner final : public XmlHandler {
 public:
  enum class Mode { Locate, Resolve };

  XPathScanner(const Mode mode, const uint32_t targetChar, const XPathStep* steps, const int stepCount)
      : mode_(mode), targetChar_(targetChar), steps_(steps), stepCount_(stepCount) {
    memset(&root_, 0, sizeof(root_));
  }

  void onStartElement(const char* name, const char** /*atts*/) override {
    const char* local = localName(name);
    if (isSuppressedElement(local)) {
      ++suppress_;
      return;
    }
    if (strcmp(local, "body") == 0) {
      inBody_ = true;
      return;
    }
    if (!inBody_) return;

    // Ancestry: sibling index among same-tag children of the current parent.
    if (depth_ < kMaxDepth) {
      AncestryLevel& parent = depth_ == 0 ? root_ : stack_[depth_ - 1];
      AncestryLevel& self = stack_[depth_];
      snprintf(self.tag, sizeof(self.tag), "%s", local);
      self.siblingIndex = bumpChildCount(parent, local);
      self.childTagKinds = 0;
    }
    ++depth_;

    if (suppress_ > 0) return;
    if (isBlockElement(local)) {
      flushParagraph();
    } else if (strcmp(local, "br") == 0) {
      ++parChars_;  // appendRaw('\n') — bypasses whitespace collapse
      checkLocate();
    } else if (strcmp(local, "hr") == 0 || strcmp(local, "img") == 0 || strcmp(local, "image") == 0) {
      flushParagraph();
    }

    if (mode_ == Mode::Resolve && !resolved_ && depth_ <= kMaxDepth && depth_ == stepCount_ && ancestryMatchesSteps()) {
      // Element start + the xpath's text offset. A pending collapsed space
      // materializes before this element's first character, so it counts.
      resolvedChar_ = charBase_ + parChars_ + (pendingSpace_ ? 1u : 0u) + targetChar_;
      resolved_ = true;
      stopParse = true;
    }
  }

  void onEndElement(const char* name) override {
    const char* local = localName(name);
    if (isSuppressedElement(local)) {
      if (suppress_ > 0) --suppress_;
      return;
    }
    if (strcmp(local, "body") == 0) {
      flushParagraph();
      inBody_ = false;
      return;
    }
    if (!inBody_) return;
    if (depth_ > 0) --depth_;
    if (suppress_ == 0 && isBlockElement(local)) flushParagraph();
  }

  void onText(const char* text, const int len) override {
    if (!inBody_ || suppress_ > 0 || stopParse) return;
    for (int i = 0; i < len; ++i) {
      const char c = text[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        pendingSpace_ = parChars_ > 0;
      } else {
        if (pendingSpace_) {
          ++parChars_;  // the collapsed space materializes
          pendingSpace_ = false;
          checkLocate();
          if (stopParse) return;
        }
        if ((static_cast<uint8_t>(c) & 0xC0) != 0x80) {
          ++parChars_;  // one codepoint (lead or ASCII byte)
          checkLocate();
          if (stopParse) return;
        }
      }
    }
  }

  bool located() const { return located_; }
  bool resolved() const { return resolved_; }
  uint32_t resolvedChar() const { return resolvedChar_; }
  uint32_t locatedOffset() const { return locatedOffset_; }
  int locatedDepth() const { return locatedDepth_; }
  const AncestryLevel* locatedStack() const { return locatedStack_; }

 private:
  int bumpChildCount(AncestryLevel& parent, const char* tag) {
    const uint32_t hash = fnvHash(tag);
    for (int i = 0; i < parent.childTagKinds; ++i) {
      if (parent.childTagHashes[i] == hash) return ++parent.childTagCounts[i];
    }
    if (parent.childTagKinds < kMaxSiblingTags) {
      parent.childTagHashes[parent.childTagKinds] = hash;
      parent.childTagCounts[parent.childTagKinds] = 1;
      ++parent.childTagKinds;
      return 1;
    }
    return 1;  // tag-table overflow: index degrades to 1 (rare, deep soup)
  }

  void flushParagraph() {
    charBase_ += parChars_;
    parChars_ = 0;
    pendingSpace_ = false;
  }

  // Locate mode: the character at index `targetChar_` was just appended —
  // capture the current ancestry and the offset within this paragraph.
  void checkLocate() {
    if (mode_ != Mode::Locate || located_) return;
    if (charBase_ + parChars_ > targetChar_) {
      locatedDepth_ = depth_ <= kMaxDepth ? depth_ : kMaxDepth;
      memcpy(locatedStack_, stack_, sizeof(AncestryLevel) * locatedDepth_);
      locatedOffset_ = targetChar_ >= charBase_ ? targetChar_ - charBase_ : 0;
      located_ = true;
      stopParse = true;
    }
  }

  bool ancestryMatchesSteps() const {
    for (int i = 0; i < stepCount_; ++i) {
      if (strcmp(stack_[i].tag, steps_[i].tag) != 0 || stack_[i].siblingIndex != steps_[i].siblingIndex) {
        return false;
      }
    }
    return true;
  }

  const Mode mode_;
  const uint32_t targetChar_;
  const XPathStep* steps_;
  const int stepCount_;

  AncestryLevel root_;
  AncestryLevel stack_[kMaxDepth];
  int depth_ = 0;
  int suppress_ = 0;
  bool inBody_ = false;

  uint32_t charBase_ = 0;
  uint32_t parChars_ = 0;
  bool pendingSpace_ = false;

  bool located_ = false;
  uint32_t locatedOffset_ = 0;
  int locatedDepth_ = 0;
  AncestryLevel locatedStack_[kMaxDepth];

  bool resolved_ = false;
  uint32_t resolvedChar_ = 0;
};

bool runScan(freeink::book::BookSource& source, const freeink::book::ZipEntry& entry, XPathScanner& scanner) {
  auto scratchBuf = makeUniqueNoThrow<uint8_t[]>(kParseScratchSize);
  if (!scratchBuf) {
    LOG_ERR("KOXP", "OOM: xpath scan scratch (%u B)", static_cast<unsigned>(kParseScratchSize));
    return false;
  }
  Arena scratch(scratchBuf.get(), kParseScratchSize);
  const BookStatus st = XmlSax::parseEntry(source, entry, scratch, scanner, /*filterHtmlEntities=*/true);
  return st == BookStatus::Ok;
}

}  // namespace

namespace BookXPath {

std::string xpathForCharStart(freeink::book::BookSource& source, const freeink::book::ZipCatalog& /*zip*/,
                              const freeink::book::ZipEntry& entry, const int spineIndex, const uint32_t charStart) {
  const std::string chapterOnly = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]";

  XPathScanner scanner(XPathScanner::Mode::Locate, charStart, nullptr, 0);
  if (!runScan(source, entry, scanner) || !scanner.located()) {
    return chapterOnly;
  }

  std::string xpath = chapterOnly + "/body";
  const AncestryLevel* stack = scanner.locatedStack();
  for (int i = 0; i < scanner.locatedDepth(); ++i) {
    xpath += "/";
    xpath += stack[i].tag;
    xpath += "[" + std::to_string(stack[i].siblingIndex) + "]";
  }
  xpath += "/text()." + std::to_string(scanner.locatedOffset());
  return xpath;
}

bool charStartForXpath(freeink::book::BookSource& source, const freeink::book::ZipCatalog& /*zip*/,
                       const freeink::book::ZipEntry& entry, const std::string& xpath, uint32_t* charStartOut) {
  *charStartOut = 0;
  XPathStep steps[kMaxSteps];
  int charOffset = 0;
  const int stepCount = parseXPathSteps(xpath, steps, &charOffset);
  if (stepCount == 0) {
    // "/body/DocFragment[N]" with no element steps = chapter start.
    return spineIndexForXpath(xpath) >= 0;
  }

  XPathScanner scanner(XPathScanner::Mode::Resolve, static_cast<uint32_t>(charOffset), steps, stepCount);
  if (!runScan(source, entry, scanner) || !scanner.resolved()) {
    return false;
  }
  *charStartOut = scanner.resolvedChar();
  return true;
}

int spineIndexForXpath(const std::string& xpath) {
  static const char kFrag[] = "/body/DocFragment[";
  const size_t pos = xpath.find(kFrag);
  if (pos == std::string::npos) return -1;
  int val = 0;
  bool any = false;
  for (size_t i = pos + strlen(kFrag); i < xpath.size() && xpath[i] != ']'; ++i) {
    if (xpath[i] < '0' || xpath[i] > '9') return -1;
    val = val * 10 + (xpath[i] - '0');
    any = true;
  }
  return any && val > 0 ? val - 1 : -1;
}

}  // namespace BookXPath
