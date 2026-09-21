set(LLVM_REQUIRES_EH ON)
add_llvm_executable(ae PARTIAL_SOURCES_INTENDED ae.cpp)
if(SVF_BUILD_BOX_STORAGE_OBSERVER)
  set(BOX_STORAGE_OBSERVER_SOURCES box-storage-observer.cpp)
  if(SVF_BOX_STORAGE_TELEMETRY)
    list(APPEND BOX_STORAGE_OBSERVER_SOURCES box-operation-census.cpp)
  endif()
  add_llvm_executable(box-storage-observer PARTIAL_SOURCES_INTENDED
    ${BOX_STORAGE_OBSERVER_SOURCES})
  target_compile_definitions(box-storage-observer PRIVATE AUDIT_BOX=1)
  if(SVF_BOX_STORAGE_TELEMETRY)
    add_test(NAME box-storage-occupancy-contract
      COMMAND box-storage-observer --storage-occupancy-self-test)
  endif()
endif()
if(SVF_BUILD_BOX_PAGE_TESTS)
  add_llvm_executable(box-page-semantic-observer PARTIAL_SOURCES_INTENDED
    ${SVF_SOURCE_DIR}/scripts/t5-semantic-observer.cpp)
  target_compile_definitions(box-page-semantic-observer PRIVATE AUDIT_BOX=1 SVF_BOX_PAGE_IDENTITY)
  add_test(NAME box-adapter-layout-contract
    COMMAND box-page-semantic-observer --adapter-contract
      -extapi=${SVF_BINARY_DIR}/lib/extapi.bc
      ${SVF_SOURCE_DIR}/scripts/box-adapter-contract.ll)
endif()
