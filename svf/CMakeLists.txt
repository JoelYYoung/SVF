# Build the backend-neutral abstract-domain library as a separate target. None of
# these sources includes SVF IR or LLVM headers.
set(ABSTRACT_DOMAIN_CORE_HEADERS
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/DimensionLayout.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/OctagonDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/ConvexPolyhedraDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/ELINABackendContract.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/AbstractDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/Variable.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/Expression.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/NumericalDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/NumericalDomainFactory.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/NumericalOperationTrace.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/PartialRelationalDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/AddressDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/InitializationDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/BoxAddressDomain.h
)
set(ABSTRACT_DOMAIN_CORE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/OctagonDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/ConvexPolyhedraDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/ELINABackendContract.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/AbstractDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/AddressDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/InitializationDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/BoxAddressDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/Expression.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/NumericalDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/NumericalDomainFactory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/NumericalOperationTrace.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/PartialRelationalDomain.cpp
)
add_library(AbstractDomainCore)
add_library(SVF::AbstractDomainCore ALIAS AbstractDomainCore)
target_compile_features(AbstractDomainCore PUBLIC cxx_std_17)
target_sources(
  AbstractDomainCore
  PUBLIC FILE_SET HEADERS
                  BASE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include
                  FILES ${ABSTRACT_DOMAIN_CORE_HEADERS}
  PRIVATE ${ABSTRACT_DOMAIN_CORE_SOURCES}
)
target_include_directories(
  AbstractDomainCore PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
)
target_link_libraries(AbstractDomainCore PUBLIC GMP::GMPXX MPFR::MPFR)

option(SVF_ENABLE_ELINA "Build optional ELINA numerical-domain backends" OFF)
set(ELINA_ROOT "" CACHE PATH "Fixed ELINA installation root")
set(ELINA_SOURCE_ROOT "${ELINA_ROOT}" CACHE PATH
    "Fixed ELINA source checkout used to verify the pinned commit")
if(SVF_ENABLE_ELINA)
  set(SVF_ELINA_REQUIRED_COMMIT
      "f524156d292ac3a6f3cd676e2d2e7db6629e2b6f")
  if(NOT ELINA_ROOT OR NOT ELINA_SOURCE_ROOT)
    message(FATAL_ERROR
            "SVF_ENABLE_ELINA requires ELINA_ROOT and the fixed "
            "ELINA_SOURCE_ROOT checkout")
  endif()
  find_package(Git REQUIRED)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${ELINA_SOURCE_ROOT}" rev-parse HEAD
    RESULT_VARIABLE ELINA_GIT_RESULT
    OUTPUT_VARIABLE ELINA_GIT_COMMIT
    ERROR_VARIABLE ELINA_GIT_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT ELINA_GIT_RESULT EQUAL 0)
    message(FATAL_ERROR
            "ELINA_SOURCE_ROOT must be the fixed ELINA Git checkout: ${ELINA_GIT_ERROR}")
  endif()
  if(NOT ELINA_GIT_COMMIT STREQUAL SVF_ELINA_REQUIRED_COMMIT)
    message(FATAL_ERROR
            "ELINA source mismatch: expected ${SVF_ELINA_REQUIRED_COMMIT}, "
            "found ${ELINA_GIT_COMMIT}")
  endif()
  find_path(ELINA_AUX_INCLUDE_DIR elina_abstract0.h
            HINTS ${ELINA_ROOT} PATH_SUFFIXES include elina_auxiliary)
  find_path(ELINA_OCT_INCLUDE_DIR opt_oct.h
            HINTS ${ELINA_ROOT} PATH_SUFFIXES include elina_oct)
  find_path(ELINA_POLY_INCLUDE_DIR opt_pk.h
            HINTS ${ELINA_ROOT} PATH_SUFFIXES include elina_poly)
  find_path(ELINA_LINEARIZE_INCLUDE_DIR elina_generic.h
            HINTS ${ELINA_ROOT} PATH_SUFFIXES include elina_linearize)
  find_path(ELINA_PARTITIONS_INCLUDE_DIR comp_list.h
            HINTS ${ELINA_ROOT} PATH_SUFFIXES include partitions_api)
  find_library(ELINA_OCT_LIBRARY NAMES optoct liboptoct
               HINTS ${ELINA_ROOT} PATH_SUFFIXES lib elina_oct)
  find_library(ELINA_POLY_LIBRARY NAMES optpoly liboptpoly
               HINTS ${ELINA_ROOT} PATH_SUFFIXES lib elina_poly)
  find_library(ELINA_AUX_LIBRARY NAMES elinaux libelinaux
               HINTS ${ELINA_ROOT} PATH_SUFFIXES lib elina_auxiliary)
  find_library(ELINA_LINEARIZE_LIBRARY NAMES elinalinearize libelinalinearize
               HINTS ${ELINA_ROOT} PATH_SUFFIXES lib elina_linearize)
  find_library(ELINA_PARTITIONS_LIBRARY NAMES partitions libpartitions
               HINTS ${ELINA_ROOT} PATH_SUFFIXES lib partitions_api)
  if(NOT ELINA_AUX_INCLUDE_DIR OR NOT ELINA_OCT_INCLUDE_DIR OR
     NOT ELINA_POLY_INCLUDE_DIR OR
     NOT ELINA_LINEARIZE_INCLUDE_DIR OR NOT ELINA_PARTITIONS_INCLUDE_DIR OR
     NOT ELINA_OCT_LIBRARY OR NOT ELINA_POLY_LIBRARY OR NOT ELINA_AUX_LIBRARY OR
     NOT ELINA_LINEARIZE_LIBRARY OR NOT ELINA_PARTITIONS_LIBRARY)
    message(FATAL_ERROR
            "SVF_ENABLE_ELINA requires fixed Octagon/Polyhedra headers and libraries under ELINA_ROOT")
  endif()
  set(ELINA_DOMAIN_CORE_SOURCES
      ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/ElinaOctagonDomain.cpp
      ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/ELINAPolyhedraDomain.cpp)
  set(ELINA_DOMAIN_CORE_HEADERS
      ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/ElinaOctagonDomain.h
      ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/ELINAPolyhedraDomain.h)
  list(APPEND ABSTRACT_DOMAIN_CORE_SOURCES ${ELINA_DOMAIN_CORE_SOURCES})
  list(APPEND ABSTRACT_DOMAIN_CORE_HEADERS ${ELINA_DOMAIN_CORE_HEADERS})
  target_sources(AbstractDomainCore PRIVATE ${ELINA_DOMAIN_CORE_SOURCES})
  target_sources(AbstractDomainCore PUBLIC FILE_SET HEADERS FILES
                 ${ELINA_DOMAIN_CORE_HEADERS})
  target_compile_definitions(AbstractDomainCore PUBLIC SVF_HAVE_ELINA=1
                                                       SVF_HAS_ELINA_POLYHEDRA=1)
  target_include_directories(AbstractDomainCore SYSTEM PRIVATE
                             ${ELINA_AUX_INCLUDE_DIR} ${ELINA_OCT_INCLUDE_DIR}
                             ${ELINA_POLY_INCLUDE_DIR}
                             ${ELINA_LINEARIZE_INCLUDE_DIR}
                             ${ELINA_PARTITIONS_INCLUDE_DIR})
  target_link_libraries(AbstractDomainCore PRIVATE
                        ${ELINA_OCT_LIBRARY} ${ELINA_POLY_LIBRARY}
                        ${ELINA_PARTITIONS_LIBRARY}
                        ${ELINA_AUX_LIBRARY} ${ELINA_LINEARIZE_LIBRARY})
endif()

if(BUILD_TESTING)
  add_executable(
    NumericalOperationTraceTest
    ${CMAKE_CURRENT_LIST_DIR}/../tests/ae-relational/NumericalOperationTraceTest.cpp
  )
  target_link_libraries(NumericalOperationTraceTest PRIVATE AbstractDomainCore)
  add_test(NAME ae-relational/NumericalOperationTraceTest
           COMMAND NumericalOperationTraceTest)
  add_executable(
    NumericalOperationTraceReplay
    ${CMAKE_CURRENT_LIST_DIR}/../tests/ae-relational/NumericalOperationTraceReplay.cpp
  )
  target_link_libraries(NumericalOperationTraceReplay PRIVATE AbstractDomainCore)
  add_executable(
    NumericalDomainOperationBenchmark
    ${CMAKE_CURRENT_LIST_DIR}/../tests/ae-relational/NumericalDomainOperationBenchmark.cpp
  )
  target_link_libraries(NumericalDomainOperationBenchmark
                        PRIVATE AbstractDomainCore)
  add_test(NAME ae-relational/NumericalDomainOperationBenchmark-smoke
           COMMAND NumericalDomainOperationBenchmark
                   --backend native --domain octagon --dimension 4
                   --shape sparse --iterations 1)
  set_tests_properties(
    ae-relational/NumericalDomainOperationBenchmark-smoke
    PROPERTIES PASS_REGULAR_EXPRESSION "construct/import" TIMEOUT 30)
  add_test(
    NAME ae-relational/MicrobenchRunnerTest
    COMMAND
      ${CMAKE_CURRENT_LIST_DIR}/../tests/ae-relational/MicrobenchRunnerTest.sh
      ${CMAKE_CURRENT_LIST_DIR}/../tests/ae-relational/run_numerical_domain_microbench.sh
      ${CMAKE_CURRENT_LIST_DIR}/../tests/ae-relational/MicrobenchRunnerFixture.sh)
  set_tests_properties(ae-relational/MicrobenchRunnerTest
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
  add_executable(
    ELINAOctagonAdapterTest
    ${CMAKE_CURRENT_LIST_DIR}/../tests/ae-relational/ELINAOctagonAdapterTest.cpp
  )
  target_link_libraries(ELINAOctagonAdapterTest PRIVATE AbstractDomainCore)
  add_test(NAME ae-relational/ELINAOctagonAdapterTest
           COMMAND ELINAOctagonAdapterTest)
  add_executable(
    ELINABackendContractTest
    ${CMAKE_CURRENT_LIST_DIR}/../tests/ae-relational/ELINABackendContractTest.cpp
  )
  target_link_libraries(ELINABackendContractTest PRIVATE AbstractDomainCore)
  add_test(NAME ae-elina-backend-contract COMMAND ELINABackendContractTest)
  set(APRON_ORACLE_ROOT "" CACHE PATH
      "Optional fixed APRON prefix for ELINA differential tests")
  if(SVF_ENABLE_ELINA AND APRON_ORACLE_ROOT AND
     EXISTS "${APRON_ORACLE_ROOT}/include/ap_abstract0.h")
    add_executable(
      ELINAApronDifferentialTest
      ${CMAKE_CURRENT_LIST_DIR}/../tests/ae-relational/ELINAApronDifferentialTest.cpp
    )
    target_include_directories(ELINAApronDifferentialTest SYSTEM PRIVATE
                               ${APRON_ORACLE_ROOT}/include)
    target_link_libraries(ELINAApronDifferentialTest PRIVATE
                          AbstractDomainCore
                          ${APRON_ORACLE_ROOT}/lib/libpolkaMPQ.so
                          ${APRON_ORACLE_ROOT}/lib/liboctMPQ.so
                          ${APRON_ORACLE_ROOT}/lib/libapron.so)
    add_test(NAME ae-elina-apron-differential
             COMMAND ELINAApronDifferentialTest)
  endif()
endif()
set_target_properties(
  AbstractDomainCore PROPERTIES
  VERSION ${SVF_VERSION}
  SOVERSION ${SVF_VERSION_MAJOR}
)

install(
  TARGETS AbstractDomainCore
  EXPORT SVFTargets
  RUNTIME DESTINATION ${SVF_INSTALL_BINDIR}
  LIBRARY DESTINATION ${SVF_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${SVF_INSTALL_LIBDIR}
          FILE_SET HEADERS
          DESTINATION ${SVF_INSTALL_INCLUDEDIR}
)

# Define the core SVF library.
add_library(SvfCore)

# Inherit compiler & linker options by publically linking against the interface library
target_link_libraries(SvfCore PUBLIC SvfFlags)

# Gather & set all of the core library's source files by globbing all .h and .cpp files (recursively)
file(GLOB_RECURSE SVF_CORE_HEADERS ${CMAKE_CURRENT_LIST_DIR}/include/*.h)
file(GLOB_RECURSE SVF_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/lib/*.cpp)
list(REMOVE_ITEM SVF_CORE_HEADERS ${ABSTRACT_DOMAIN_CORE_HEADERS})
list(REMOVE_ITEM SVF_CORE_SOURCES ${ABSTRACT_DOMAIN_CORE_SOURCES})
target_link_libraries(SvfCore PUBLIC AbstractDomainCore)
target_sources(
  SvfCore
  PUBLIC FILE_SET HEADERS
                  BASE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include
                  FILES ${SVF_CORE_HEADERS}
  PRIVATE ${SVF_CORE_SOURCES}
)

# Only expose the headers in the source tree to in-tree users of SVF
target_include_directories(SvfCore PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>)

# Set the library & .so version of the core library
set_target_properties(SvfCore PROPERTIES VERSION ${SVF_VERSION} SOVERSION ${SVF_VERSION_MAJOR})

# Install the core library's files to the regular/configured install tree
install(
  TARGETS SvfCore
  EXPORT SVFTargets
  RUNTIME DESTINATION ${SVF_INSTALL_BINDIR}
  LIBRARY DESTINATION ${SVF_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${SVF_INSTALL_LIBDIR}
          FILE_SET HEADERS
          DESTINATION ${SVF_INSTALL_INCLUDEDIR}
)
