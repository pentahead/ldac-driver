# Third-party codec libraries, built unmodified from upstream sources.
#   libldac : AOSP platform/external/libldac (Apache-2.0, Sony). See THIRD_PARTY_NOTICES.md.
#   libsbc  : github.com/google/libsbc        (Apache-2.0, Google).

# ---- libsbc -----------------------------------------------------------------
# MSVC (14.36) lacks C23 empty initialisers "{ }"; a build-time copy rewrites the two occurrences
# to the equivalent C99 "{ 0 }". The upstream file is not modified (see THIRD_PARTY_NOTICES.md).
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/libsbc/src/sbc.c SBC_SRC)
string(REPLACE "{ }" "{ 0 }" SBC_SRC "${SBC_SRC}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sbc_msvc.c "${SBC_SRC}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libsbc/src/sbc.c)
add_library(tp_sbc STATIC
  ${CMAKE_CURRENT_BINARY_DIR}/sbc_msvc.c
  libsbc/src/bits.c)
target_include_directories(tp_sbc PUBLIC libsbc/include PRIVATE libsbc/src)
# MSVC has no __builtin_clz; the shim is force-included so upstream files stay untouched.
target_compile_options(tp_sbc PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc_builtins.h /W0)

# ---- libldac (encoder) ------------------------------------------------------
add_library(tp_ldac STATIC
  libldac/src/ldacBT.c
  libldac/src/ldaclib.c)
target_include_directories(tp_ldac PUBLIC libldac/inc PRIVATE libldac/src compat/posix)
# libldac is POSIX-flavoured: <unistd.h> (unused) and __attribute__((unused)).
target_compile_options(tp_ldac PRIVATE /W0 /FI${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc_builtins.h)

# ---- libldac ABR (adaptive bitrate helper from the same AOSP project) -------
add_library(tp_ldac_abr STATIC libldac/abr/src/ldacBT_abr.c)
target_include_directories(tp_ldac_abr PUBLIC libldac/abr/inc)
target_link_libraries(tp_ldac_abr PUBLIC tp_ldac)
target_include_directories(tp_ldac_abr PRIVATE compat/posix)
target_compile_options(tp_ldac_abr PRIVATE /W0 /FI${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc_builtins.h)
