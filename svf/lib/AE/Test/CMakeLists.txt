if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.23)
  project(SVFRelationalDomainTests LANGUAGES CXX)
  include(CTest)

  get_filename_component(SVF_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
  list(APPEND CMAKE_MODULE_PATH "${SVF_REPOSITORY_ROOT}/cmake/Modules")
  find_package(GMP REQUIRED)
  find_package(MPFR REQUIRED)
  find_package(Z3 REQUIRED)

  set(RELATIONAL_STANDALONE_ROOT "${SVF_REPOSITORY_ROOT}/svf")
  add_library(
    RelationalDomainCore
    ${RELATIONAL_STANDALONE_ROOT}/lib/AE/Core/OctagonDomain.cpp
    ${RELATIONAL_STANDALONE_ROOT}/lib/AE/Core/AbstractDomain.cpp
    ${RELATIONAL_STANDALONE_ROOT}/lib/AE/Core/RelationalEnvironment.cpp
    ${RELATIONAL_STANDALONE_ROOT}/lib/AE/Core/LinearConstraint.cpp
    ${RELATIONAL_STANDALONE_ROOT}/lib/AE/Core/RelationalNumeric.cpp
  )
  target_compile_features(RelationalDomainCore PUBLIC cxx_std_17)
  target_include_directories(
    RelationalDomainCore
    PUBLIC ${RELATIONAL_STANDALONE_ROOT}/include
    PRIVATE ${RELATIONAL_STANDALONE_ROOT}/lib/AE/Core
  )
  target_link_libraries(RelationalDomainCore PUBLIC GMP::GMPXX MPFR::MPFR)
endif()

add_executable(
  svf-relational-domain-test
  RelationalDomainTest.cpp
  Z3SoundnessChecker.cpp
  Z3SoundnessChecker.h
)

target_include_directories(
  svf-relational-domain-test PRIVATE ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(
  svf-relational-domain-test PRIVATE RelationalDomainCore ${Z3_LIBRARIES}
)

add_test(NAME relational-domain-soundness COMMAND svf-relational-domain-test)

add_executable(
  svf-relational-bridge-test
  SVFRelationalBridgeTest.cpp
  Z3SoundnessChecker.cpp
  Z3SoundnessChecker.h
  ${CMAKE_CURRENT_LIST_DIR}/../Core/SVFRelationalBridge.cpp
)
target_include_directories(
  svf-relational-bridge-test PRIVATE ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(
  svf-relational-bridge-test PRIVATE RelationalDomainCore ${Z3_LIBRARIES}
)
add_test(NAME relational-domain-svf-bridge COMMAND svf-relational-bridge-test)

if(TARGET SvfLLVM)
  add_executable(
    svf-ae-relational-integration-test
    AERelationalIntegrationTest.cpp
  )
  target_link_libraries(
    svf-ae-relational-integration-test PRIVATE SvfLLVM
  )
  function(add_relational_ae_integration_test test_name sparsity)
    add_test(
      NAME ${test_name}
      COMMAND svf-ae-relational-integration-test
              -ae-relational=true
              -ae-sparsity=${sparsity}
              -stat=false
              -extapi=${CMAKE_BINARY_DIR}/lib/extapi.bc
              ${CMAKE_CURRENT_LIST_DIR}/RelationalReducedProduct.ll
    )
  endfunction()
  add_relational_ae_integration_test(
    relational-domain-ae-integration dense
  )
  add_relational_ae_integration_test(
    relational-domain-ae-integration-semi-sparse semi-sparse
  )
  add_relational_ae_integration_test(
    relational-domain-ae-integration-sparse sparse
  )
endif()

# Some standalone macOS Z3 packages use the bare install name
# "libz3.dylib". Keep the workaround local to test executables and do not
# mutate the supplied dependency.
if(APPLE)
  if(TARGET z3::libz3)
    get_target_property(Z3_TEST_LIBRARY z3::libz3 IMPORTED_LOCATION)
  else()
    list(GET Z3_LIBRARIES 0 Z3_TEST_LIBRARY)
  endif()
  get_filename_component(
    Z3_TEST_LIBRARY_DIRECTORY "${Z3_TEST_LIBRARY}" DIRECTORY
  )
  set(
    relational_test_targets
    svf-relational-domain-test
    svf-relational-bridge-test
  )
  if(TARGET svf-ae-relational-integration-test)
    list(APPEND relational_test_targets svf-ae-relational-integration-test)
  endif()
  foreach(relational_test_target IN LISTS relational_test_targets)
    set_property(
      TARGET ${relational_test_target}
      APPEND
      PROPERTY BUILD_RPATH "${Z3_TEST_LIBRARY_DIRECTORY}"
    )
    add_custom_command(
      TARGET ${relational_test_target}
      POST_BUILD
      COMMAND install_name_tool -change libz3.dylib @rpath/libz3.dylib
              "$<TARGET_FILE:${relational_test_target}>"
      VERBATIM
    )
  endforeach()
endif()
