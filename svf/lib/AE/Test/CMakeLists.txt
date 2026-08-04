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
    ${RELATIONAL_STANDALONE_ROOT}/lib/AE/Core/RelationalDomain.cpp
    ${RELATIONAL_STANDALONE_ROOT}/lib/AE/Core/RelationalEnvironment.cpp
    ${RELATIONAL_STANDALONE_ROOT}/lib/AE/Core/RelationalExpression.cpp
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
