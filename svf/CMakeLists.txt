# Build the backend-neutral relational library as a separate target.  None of
# these sources includes SVF IR or LLVM headers.
set(RELATIONAL_CORE_HEADERS
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/AbstractDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/RelationalEnvironment.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/LinearConstraint.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/OctagonDomain.h
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/RelationalNumeric.h
)
set(RELATIONAL_CORE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/OctagonDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/AbstractDomain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/RelationalEnvironment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/LinearConstraint.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/RelationalNumeric.cpp
)
set(RELATIONAL_BRIDGE_HEADERS
    ${CMAKE_CURRENT_LIST_DIR}/include/AE/Core/SVFRelationalBridge.h
)
set(RELATIONAL_BRIDGE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/lib/AE/Core/SVFRelationalBridge.cpp
)

if(SVF_BUILD_RELATIONAL_DOMAIN)

  add_library(RelationalDomainCore)
  add_library(SVF::RelationalDomainCore ALIAS RelationalDomainCore)
  target_compile_features(RelationalDomainCore PUBLIC cxx_std_17)
  target_sources(
    RelationalDomainCore
    PUBLIC FILE_SET HEADERS
                    BASE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include
                    FILES ${RELATIONAL_CORE_HEADERS}
    PRIVATE ${RELATIONAL_CORE_SOURCES}
  )
  target_include_directories(
    RelationalDomainCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
  )
  target_link_libraries(RelationalDomainCore PUBLIC GMP::GMPXX MPFR::MPFR)
  set_target_properties(
    RelationalDomainCore PROPERTIES
    VERSION ${SVF_VERSION}
    SOVERSION ${SVF_VERSION_MAJOR}
  )

  install(
    TARGETS RelationalDomainCore
    EXPORT SVFTargets
    RUNTIME DESTINATION ${SVF_INSTALL_BINDIR}
    LIBRARY DESTINATION ${SVF_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${SVF_INSTALL_LIBDIR}
            FILE_SET HEADERS
            DESTINATION ${SVF_INSTALL_INCLUDEDIR}
  )
endif()

# Define the core SVF library.
add_library(SvfCore)

# Inherit compiler & linker options by publically linking against the interface library
target_link_libraries(SvfCore PUBLIC SvfFlags)

# Gather & set all of the core library's source files by globbing all .h and .cpp files (recursively)
file(GLOB_RECURSE SVF_CORE_HEADERS ${CMAKE_CURRENT_LIST_DIR}/include/*.h)
file(GLOB_RECURSE SVF_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/lib/*.cpp)
list(FILTER SVF_CORE_SOURCES EXCLUDE REGEX "/lib/AE/Test/")
list(REMOVE_ITEM SVF_CORE_HEADERS ${RELATIONAL_CORE_HEADERS})
list(REMOVE_ITEM SVF_CORE_SOURCES ${RELATIONAL_CORE_SOURCES})
if(SVF_BUILD_RELATIONAL_DOMAIN)
  target_link_libraries(SvfCore PUBLIC RelationalDomainCore)
else()
  list(REMOVE_ITEM SVF_CORE_HEADERS ${RELATIONAL_BRIDGE_HEADERS})
  list(REMOVE_ITEM SVF_CORE_SOURCES ${RELATIONAL_BRIDGE_SOURCES})
endif()
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

if(SVF_BUILD_RELATIONAL_DOMAIN AND SVF_BUILD_RELATIONAL_TESTS)
  add_subdirectory(lib/AE/Test)
endif()
