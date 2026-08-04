# FindApron.cmake - locate APRON's built-in NewPolka domain and dependencies.
#
# Inputs:
#   Apron_ROOT / APRON_ROOT / $APRON_ROOT
#
# Result:
#   Apron_FOUND
#   Apron::Apron

find_path(
  APRON_INCLUDE_DIR
  NAMES ap_abstract0.h
  HINTS ${Apron_ROOT} ${APRON_ROOT} $ENV{APRON_ROOT}
  PATH_SUFFIXES include)

find_library(
  APRON_LIBRARY
  NAMES apron
  HINTS ${Apron_ROOT} ${APRON_ROOT} $ENV{APRON_ROOT}
  PATH_SUFFIXES lib)

find_library(
  APRON_NEWPOLKA_LIBRARY
  NAMES polkaMPQ
  HINTS ${Apron_ROOT} ${APRON_ROOT} $ENV{APRON_ROOT}
  PATH_SUFFIXES lib)

find_library(
  APRON_MPFR_LIBRARY
  NAMES mpfr
  HINTS ${Apron_ROOT} ${APRON_ROOT} $ENV{APRON_ROOT}
  PATH_SUFFIXES lib)

find_library(
  APRON_GMP_LIBRARY
  NAMES gmp
  HINTS ${Apron_ROOT} ${APRON_ROOT} $ENV{APRON_ROOT}
  PATH_SUFFIXES lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  Apron
  REQUIRED_VARS APRON_INCLUDE_DIR APRON_LIBRARY APRON_NEWPOLKA_LIBRARY APRON_MPFR_LIBRARY
                APRON_GMP_LIBRARY)

if(Apron_FOUND AND NOT TARGET Apron::Apron)
  add_library(Apron::Apron INTERFACE IMPORTED)
  set_target_properties(
    Apron::Apron
    PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${APRON_INCLUDE_DIR}"
      # Static link order matters: NewPolka -> APRON -> MPFR -> GMP.
      INTERFACE_LINK_LIBRARIES
      "${APRON_NEWPOLKA_LIBRARY};${APRON_LIBRARY};${APRON_MPFR_LIBRARY};${APRON_GMP_LIBRARY};m")
endif()

mark_as_advanced(
  APRON_INCLUDE_DIR APRON_LIBRARY APRON_NEWPOLKA_LIBRARY APRON_MPFR_LIBRARY
  APRON_GMP_LIBRARY)
