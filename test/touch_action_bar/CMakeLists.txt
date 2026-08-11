add_executable(TouchActionBarGeometryTest
  TouchActionBarGeometryTest.cpp
)

target_link_libraries(TouchActionBarGeometryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TouchActionBarGeometryTest)
