# Build for the DMS runtime support library.
# This CMakeLists.txt adapted from the ASAN one, may explain some peculiarities here

set(DMS_SOURCES
  DMS.cpp
  )

SET(DMS_HEADERS
  dms_interface.h
  )

include_directories(..)

set(DMS_CFLAGS ${SANITIZER_COMMON_CFLAGS})

append_rtti_flag(OFF DMS_CFLAGS)

set(DMS_DYNAMIC_LINK_FLAGS ${SANITIZER_COMMON_LINK_FLAGS})

if(ANDROID)
# Put most Sanitizer shared libraries in the global group. For more details, see
# android-changes-for-ndk-developers.md#changes-to-library-search-order
  if (COMPILER_RT_HAS_Z_GLOBAL)
    list(APPEND DMS_DYNAMIC_LINK_FLAGS -Wl,-z,global)
  endif()
endif()

set(DMS_DYNAMIC_DEFINITIONS
  ${DMS_COMMON_DEFINITIONS} DMS_DYNAMIC=1)
append_list_if(WIN32 INTERCEPTION_DYNAMIC_CRT DMS_DYNAMIC_DEFINITIONS)

set(DMS_DYNAMIC_CFLAGS ${DMS_CFLAGS})
append_list_if(COMPILER_RT_HAS_FTLS_MODEL_INITIAL_EXEC
  -ftls-model=initial-exec DMS_DYNAMIC_CFLAGS)
append_list_if(MSVC /DEBUG DMS_DYNAMIC_LINK_FLAGS)

set(DMS_DYNAMIC_LIBS ${SANITIZER_CXX_ABI_LIBRARIES} ${SANITIZER_COMMON_LINK_LIBS})

append_list_if(COMPILER_RT_HAS_LIBDL dl DMS_DYNAMIC_LIBS)
append_list_if(COMPILER_RT_HAS_LIBRT rt DMS_DYNAMIC_LIBS)
append_list_if(COMPILER_RT_HAS_LIBM m DMS_DYNAMIC_LIBS)
append_list_if(COMPILER_RT_HAS_LIBPTHREAD pthread DMS_DYNAMIC_LIBS)
append_list_if(COMPILER_RT_HAS_LIBLOG log DMS_DYNAMIC_LIBS)
append_list_if(MINGW "${MINGW_LIBRARIES}" DMS_DYNAMIC_LIBS)

# Compile DMS sources into an object library.

add_compiler_rt_object_libraries(RTDMS_dynamic
  OS ${SANITIZER_COMMON_SUPPORTED_OS}
  ARCHS ${ASAN_SUPPORTED_ARCH} # just use the same archs as asan
  SOURCES ${DMS_SOURCES}
  ADDITIONAL_HEADERS ${DMS_HEADERS}
  CFLAGS ${DMS_DYNAMIC_CFLAGS}
  DEFS ${DMS_DYNAMIC_DEFINITIONS})

if(NOT APPLE)
  add_compiler_rt_object_libraries(RTDMS
    ARCHS ${ASAN_SUPPORTED_ARCH} # just use the same archs as asan
    SOURCES ${DMS_SOURCES}
    ADDITIONAL_HEADERS ${DMS_HEADERS}
    CFLAGS ${DMS_CFLAGS}
    DEFS ${DMS_COMMON_DEFINITIONS})
endif()

# Build DMS runtimes shipped with Clang.
add_compiler_rt_component(dms)

if(APPLE)
  add_weak_symbols("dms" WEAK_SYMBOL_LINK_FLAGS)
  add_weak_symbols("asan" WEAK_SYMBOL_LINK_FLAGS)
  add_weak_symbols("lsan" WEAK_SYMBOL_LINK_FLAGS)
  add_weak_symbols("ubsan" WEAK_SYMBOL_LINK_FLAGS)
  add_weak_symbols("sanitizer_common" WEAK_SYMBOL_LINK_FLAGS)
  add_weak_symbols("xray" WEAK_SYMBOL_LINK_FLAGS)

  add_compiler_rt_runtime(clang_rt.dms
    SHARED
    OS ${SANITIZER_COMMON_SUPPORTED_OS}
    ARCHS ${ASAN_SUPPORTED_ARCH} # just use the same archs as asan
    OBJECT_LIBS RTDMS_dynamic
                RTDMS
                RTInterception
                RTSanitizerCommon
                RTSanitizerCommonLibc
                RTSanitizerCommonCoverage
                RTSanitizerCommonSymbolizer
                #RTLSanCommon
                #RTUbsan
    CFLAGS ${DMS_DYNAMIC_CFLAGS}
    LINK_FLAGS ${WEAK_SYMBOL_LINK_FLAGS}
    DEFS ${DMS_DYNAMIC_DEFINITIONS}
    PARENT_TARGET dms)
else()
  # Build separate libraries for each target.

  set(DMS_COMMON_RUNTIME_OBJECT_LIBS
    RTInterception
    RTSanitizerCommon
    RTSanitizerCommonLibc
    RTSanitizerCommonCoverage
    RTSanitizerCommonSymbolizer
    #RTLSanCommon
    #RTUbsan
    )

  add_compiler_rt_runtime(clang_rt.dms
    STATIC
    ARCHS ${ASAN_SUPPORTED_ARCH} # just use the same archs as asan
    OBJECT_LIBS RTDMS
                ${DMS_COMMON_RUNTIME_OBJECT_LIBS}
    CFLAGS ${DMS_CFLAGS}
    DEFS ${DMS_COMMON_DEFINITIONS}
    PARENT_TARGET dms)

  foreach(arch ${ASAN_SUPPORTED_ARCH}) # just use the same archs as asan
    if (COMPILER_RT_HAS_VERSION_SCRIPT)
      add_sanitizer_rt_version_list(clang_rt.dms-dynamic-${arch}
                                    LIBS clang_rt.dms-${arch}
                                    EXTRA dms.syms.extra)
      set(VERSION_SCRIPT_FLAG
           -Wl,--version-script,${CMAKE_CURRENT_BINARY_DIR}/clang_rt.dms-dynamic-${arch}.vers)
      # The Solaris 11.4 linker supports a subset of GNU ld version scripts,
      # but requires a special option to enable it.
      if (COMPILER_RT_HAS_GNU_VERSION_SCRIPT_COMPAT)
          list(APPEND VERSION_SCRIPT_FLAG -Wl,-z,gnu-version-script-compat)
      endif()
      set_property(SOURCE
        ${CMAKE_CURRENT_BINARY_DIR}/dummy.cpp
        APPEND PROPERTY
        OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/clang_rt.dms-dynamic-${arch}.vers)
    else()
      set(VERSION_SCRIPT_FLAG)
    endif()

    if(FALSE)
    add_compiler_rt_runtime(clang_rt.dms
      SHARED
      ARCHS ${arch}
      OBJECT_LIBS ${DMS_COMMON_RUNTIME_OBJECT_LIBS}
                  RTDMS_dynamic
                  #clang_rt.dms-dynamic-${arch}
                  #clang_rt.dms-dynamic-${arch}-version-list
      #add_dependencies(clang_rt.dms-dynamic-${arch} clang_rt.dms-dynamic-${arch}-version-list)
      CFLAGS ${DMS_DYNAMIC_CFLAGS}
      LINK_FLAGS ${DMS_DYNAMIC_LINK_FLAGS}
                ${VERSION_SCRIPT_FLAG}
      LINK_LIBS ${DMS_DYNAMIC_LIBS}
      DEFS ${DMS_DYNAMIC_DEFINITIONS}
      PARENT_TARGET dms)
    endif()

    if (SANITIZER_USE_SYMBOLS AND NOT ${arch} STREQUAL "i386")
      add_sanitizer_rt_symbols(clang_rt.dms
        ARCHS ${arch}
        EXTRA dms.syms.extra)
      add_dependencies(dms clang_rt.dms-${arch}-symbols)
    endif()
  endforeach()
endif()

# add_subdirectory(scripts)

if(COMPILER_RT_INCLUDE_TESTS)
  # add_subdirectory(tests)
endif()
