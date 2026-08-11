add_executable(DictionaryWordGeometryTest
  DictionaryWordGeometryTest.cpp
)

target_link_libraries(DictionaryWordGeometryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictionaryWordGeometryTest)
