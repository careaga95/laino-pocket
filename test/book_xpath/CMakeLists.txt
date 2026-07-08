# BookXPath maps KOReader xpaths <-> FreeInkBook character offsets by
# streaming chapters through the engine's XmlSax; the suite builds the
# relevant engine sources host-side. Same FreeInkBook discovery as
# cpfont_adapter (submodule first, sibling working checkout fallback).
set(FREEINK_BOOK_DIR "${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook")
if(NOT EXISTS "${FREEINK_BOOK_DIR}/include")
  set(FREEINK_BOOK_DIR "$ENV{HOME}/GitHub/freeink-sdk/libs/book/FreeInkBook")
endif()

if(NOT EXISTS "${FREEINK_BOOK_DIR}/include")
  message(WARNING "FreeInkBook sources not found - skipping BookXPathTest")
else()
  enable_language(C)  # vendored expat/miniz translation units
  add_executable(BookXPathTest
    BookXPathTest.cpp
    ${REPO_ROOT}/lib/KOReaderSync/BookXPath.cpp
    ${FREEINK_BOOK_DIR}/src/epub/XmlSax.cpp
    ${FREEINK_BOOK_DIR}/src/epub/ZipCatalog.cpp
    ${FREEINK_BOOK_DIR}/src/text/EntityFilter.cpp
    ${FREEINK_BOOK_DIR}/src/vendor/expat_xmlparse.c
    ${FREEINK_BOOK_DIR}/src/vendor/expat_xmlrole.c
    ${FREEINK_BOOK_DIR}/src/vendor/expat_xmltok.c
    ${FREEINK_BOOK_DIR}/src/vendor/miniz_impl.c
  )

  target_include_directories(BookXPathTest PRIVATE
    ${REPO_ROOT}/lib/KOReaderSync
    ${REPO_ROOT}/lib/Memory
    ${FREEINK_BOOK_DIR}/include
    ${FREEINK_BOOK_DIR}/third_party/expat
    ${FREEINK_BOOK_DIR}/third_party/miniz
  )

  # The firmware stubs (Logging/Memory) come from a tiny local shim dir.
  target_include_directories(BookXPathTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim)

  target_link_libraries(BookXPathTest PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )

  gtest_discover_tests(BookXPathTest)
endif()
