# Build jemalloc
if(NOT EXISTS "${JEMALLOC_LIB_PATH}/libjemalloc.so")
  execute_process(COMMAND ./autogen.sh
                  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/jemalloc)
  execute_process(
    COMMAND make -j
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/jemalloc
    RESULT_VARIABLE JEMALLOC_BUILD_RESULT)
  if(NOT JEMALLOC_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to build jemalloc")
  endif()
endif()
