add_executable(PocketCardParserTest
  PocketCardParserTest.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCard.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCardFixture.cpp
  ${REPO_ROOT}/src/activities/pocket/PocketCardParser.cpp
)

target_include_directories(PocketCardParserTest PRIVATE
  ${REPO_ROOT}/src/activities/pocket
)

target_link_libraries(PocketCardParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PocketCardParserTest)
