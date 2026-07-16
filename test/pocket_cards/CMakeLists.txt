add_executable(PocketCardsTest
  PocketCardsTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCard.cpp
)

target_include_directories(PocketCardsTest PRIVATE
  ${REPO_ROOT}/lib/I18n
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src/activities/pocket
)

target_link_libraries(PocketCardsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PocketCardsTest)
