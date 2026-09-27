execute_process(COMMAND "${PACKED_BINARY}" --mutation
  RESULT_VARIABLE exit_code OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT exit_code EQUAL 1 OR NOT error MATCHES "original packed equation mismatch")
  message(FATAL_ERROR "Mutation checker failed: ${exit_code}\n${output}\n${error}")
endif()
message(STATUS "Corrupted assume dependency rejected by original equations")
