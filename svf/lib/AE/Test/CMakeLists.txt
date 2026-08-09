if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.23)
  project(SVFAbstractDomainTests LANGUAGES CXX)
  include(CTest)

  get_filename_component(SVF_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
  list(APPEND CMAKE_MODULE_PATH "${SVF_REPOSITORY_ROOT}/cmake/Modules")
  find_package(GMP REQUIRED)
  find_package(MPFR REQUIRED)
  find_package(Z3 REQUIRED)

  set(ABSTRACT_DOMAIN_SOURCE_ROOT "${SVF_REPOSITORY_ROOT}/svf")
  add_library(
    AbstractDomainCore
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/BoxDomain.cpp
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/NonRelationalDomain.cpp
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/ConvexPolyhedraDomain.cpp
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/OctagonDomain.cpp
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/AbstractState.cpp
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/VariableEnvironment.cpp
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/LinearConstraint.cpp
    ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core/NumericPrimitives.cpp
  )
  target_compile_features(AbstractDomainCore PUBLIC cxx_std_17)
  target_include_directories(
    AbstractDomainCore
    PUBLIC ${ABSTRACT_DOMAIN_SOURCE_ROOT}/include
    PRIVATE ${ABSTRACT_DOMAIN_SOURCE_ROOT}/lib/AE/Core
  )
  target_link_libraries(AbstractDomainCore PUBLIC GMP::GMPXX MPFR::MPFR)
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
  svf-relational-domain-test PRIVATE AbstractDomainCore ${Z3_LIBRARIES}
)

add_test(NAME relational-domain-soundness COMMAND svf-relational-domain-test)

add_executable(
  svf-abstract-domain-state-test
  AbstractDomainStateTest.cpp
  Z3SoundnessChecker.cpp
  Z3SoundnessChecker.h
)
target_include_directories(
  svf-abstract-domain-state-test PRIVATE ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(
  svf-abstract-domain-state-test PRIVATE AbstractDomainCore ${Z3_LIBRARIES}
)
add_test(NAME abstract-domain-state COMMAND svf-abstract-domain-state-test)

if(TARGET SvfCore)
  add_executable(
    svf-interval-state-test
    IntervalStateTest.cpp
  )
  target_link_libraries(svf-interval-state-test PRIVATE SvfCore)
  add_test(NAME interval-state COMMAND svf-interval-state-test)
endif()

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
              -ae-dense-octagon=true
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
  function(add_relational_ae_loop_test test_name sparsity)
    add_test(
      NAME ${test_name}
      COMMAND svf-ae-relational-integration-test
              -ae-dense-octagon=true
              -ae-sparsity=${sparsity}
              -widen-delay=1
              -stat=false
              -extapi=${CMAKE_BINARY_DIR}/lib/extapi.bc
              ${CMAKE_CURRENT_LIST_DIR}/RelationalLoop.ll
    )
  endfunction()
  add_relational_ae_loop_test(
    relational-domain-ae-loop dense
  )
  add_relational_ae_loop_test(
    relational-domain-ae-loop-semi-sparse semi-sparse
  )
  add_relational_ae_loop_test(
    relational-domain-ae-loop-sparse sparse
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
    abstract_domain_test_targets
    svf-relational-domain-test
    svf-abstract-domain-state-test
  )
  if(TARGET svf-ae-relational-integration-test)
    list(APPEND abstract_domain_test_targets
         svf-ae-relational-integration-test)
  endif()
  foreach(abstract_domain_test_target IN LISTS abstract_domain_test_targets)
    set_property(
      TARGET ${abstract_domain_test_target}
      APPEND
      PROPERTY BUILD_RPATH "${Z3_TEST_LIBRARY_DIRECTORY}"
    )
    add_custom_command(
      TARGET ${abstract_domain_test_target}
      POST_BUILD
      COMMAND install_name_tool -change libz3.dylib @rpath/libz3.dylib
              "$<TARGET_FILE:${abstract_domain_test_target}>"
      VERBATIM
    )
  endforeach()
endif()
