#pragma once

// KOReader xpath <-> FreeInkBook character offset, via one streaming SAX pass
// over the chapter's XHTML (no DOM, no pagination — chapter character offsets
// are layout-independent, so neither direction needs the page cache).
//
// The text accounting mirrors ChapterLayout exactly (suppressed head/style/
// script/title, body gating, whitespace collapse, <br> newlines, block
// flushes), so the offsets these functions produce address the same
// characters the engine's page records anchor on. Known approximation:
// display:none content is counted here but skipped by layout when the book's
// CSS hides it — a paragraph-level drift on such books, corrected by the
// percentage fallback.

#include <FreeInkBook.h>

#include <cstdint>
#include <string>

namespace BookXPath {

// Real-ancestry KOReader xpath for a chapter character offset:
// "/body/DocFragment[N]/body/div[2]/p[4]/text().17". Falls back to the
// chapter-level "/body/DocFragment[N]" when the offset lies past the text or
// the parse fails.
std::string xpathForCharStart(freeink::book::BookSource& source, const freeink::book::ZipCatalog& zip,
                              const freeink::book::ZipEntry& entry, int spineIndex, uint32_t charStart);

// Resolves a KOReader xpath's element ancestry to the chapter character
// offset where that element's text begins (plus the xpath's text offset).
// Returns false when the ancestry cannot be matched — caller falls back to
// the synced percentage.
bool charStartForXpath(freeink::book::BookSource& source, const freeink::book::ZipCatalog& zip,
                       const freeink::book::ZipEntry& entry, const std::string& xpath, uint32_t* charStartOut);

// The 0-based spine index from "/body/DocFragment[N]/...", or -1.
int spineIndexForXpath(const std::string& xpath);

}  // namespace BookXPath
