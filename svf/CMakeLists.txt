# Build the backend-neutral abstract-domain library as a separate target. None of
# these sources includes SVF IR or LLVM headers.
set(ABSTRACT_DOMAIN_CORE_HEADERS
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/DimensionLayout.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/OctagonDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/ConvexPolyhedraDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/ELINABackendContract.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/NumericalDomainFactory.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/AbstractDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/Variable.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/Expression.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/NumericalDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/PartialRelationalDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/AddressDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/InitializationDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/BoxAddressDomain.h
)
set(ABSTRACT_DOMAIN_CORE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/OctagonDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/ConvexPolyhedraDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/ELINABackendContract.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/NumericalDomainFactory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/AbstractDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/AddressDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/InitializationDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/BoxAddressDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/Expression.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/NumericalDomain.cpp
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

if(SVF_ENABLE_ELINA_POLYHEDRA)
  if(NOT SVF_ELINA_ROOT)
    message(FATAL_ERROR
            "SVF_ENABLE_ELINA_POLYHEDRA requires SVF_ELINA_ROOT")
  endif()
  if(NOT EXISTS "${SVF_ELINA_ROOT}/.git")
    message(FATAL_ERROR "SVF_ELINA_ROOT is not an ELINA Git checkout")
  endif()
  execute_process(
    COMMAND git -C "${SVF_ELINA_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE SVF_ELINA_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  if(NOT SVF_ELINA_COMMIT STREQUAL
         "f524156d292ac3a6f3cd676e2d2e7db6629e2b6f")
    message(FATAL_ERROR
            "ELINA adapter requires commit f524156d292ac3a6f3cd676e2d2e7db6629e2b6f")
  endif()

  find_library(SVF_ELINA_POLY_LIBRARY NAMES optpoly liboptpoly
               PATHS "${SVF_ELINA_ROOT}/elina_poly" NO_DEFAULT_PATH REQUIRED)
  find_library(SVF_ELINA_AUX_LIBRARY NAMES elinaux libelinaux
               PATHS "${SVF_ELINA_ROOT}/elina_auxiliary" NO_DEFAULT_PATH REQUIRED)
  find_library(SVF_ELINA_LINEARIZE_LIBRARY NAMES elinalinearize libelinalinearize
               PATHS "${SVF_ELINA_ROOT}/elina_linearize" NO_DEFAULT_PATH REQUIRED)
  find_library(SVF_ELINA_PARTITIONS_LIBRARY NAMES partitions libpartitions
               PATHS "${SVF_ELINA_ROOT}/partitions_api" NO_DEFAULT_PATH REQUIRED)

  target_sources(
    AbstractDomainCore
    PUBLIC FILE_SET HEADERS FILES
           ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/ELINAPolyhedraDomain.h
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/ELINAPolyhedraDomain.cpp
  )
  target_include_directories(
    AbstractDomainCore SYSTEM PRIVATE
    "${SVF_ELINA_ROOT}/elina_auxiliary"
    "${SVF_ELINA_ROOT}/elina_poly"
    "${SVF_ELINA_ROOT}/elina_linearize"
    "${SVF_ELINA_ROOT}/partitions_api"
  )
  target_compile_definitions(AbstractDomainCore PUBLIC SVF_HAS_ELINA_POLYHEDRA=1)
  target_link_libraries(
    AbstractDomainCore PRIVATE
    ${SVF_ELINA_POLY_LIBRARY}
    ${SVF_ELINA_PARTITIONS_LIBRARY}
    ${SVF_ELINA_LINEARIZE_LIBRARY}
    ${SVF_ELINA_AUX_LIBRARY}
  )
endif()

if(BUILD_TESTING)
  add_executable(
    ELINABackendContractTest
    ${CMAKE_CURRENT_LIST_DIR}/../tests/ae-relational/ELINABackendContractTest.cpp
  )
  target_link_libraries(ELINABackendContractTest PRIVATE AbstractDomainCore)
  add_test(NAME ae-elina-backend-contract COMMAND ELINABackendContractTest)
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
