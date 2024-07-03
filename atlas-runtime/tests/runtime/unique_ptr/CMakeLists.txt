add_executable(unique_ptr_test unique_ptr_test.cc)
target_link_libraries(unique_ptr_test PUBLIC paris pthread GTest::gtest)

add_custom_command(
  TARGET unique_ptr_test
  POST_BUILD
  COMMAND cp ${CMAKE_CURRENT_LIST_DIR}/test.sh ${CMAKE_CURRENT_BINARY_DIR}
  VERBATIM)

add_test(
  NAME unique_ptr_test
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  COMMAND bash ./test.sh $<TARGET_FILE:unique_ptr_test>)
