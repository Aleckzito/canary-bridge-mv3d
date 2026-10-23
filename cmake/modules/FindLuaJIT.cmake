# Once done these will be defined:
#
# LUAJIT_FOUND LUAJIT_INCLUDE_DIRS LUAJIT_LIBRARIES
#
# For use in OBS:
#
# LUAJIT_INCLUDE_DIR

if(CMAKE_SIZEOF_VOID_P
   EQUAL
   8
)
    set(_LIB_SUFFIX
        64
    )
else()
    set(_LIB_SUFFIX
        32
    )
endif()

if(DEFINED VCPKG_INSTALLED_DIR
   AND DEFINED VCPKG_TARGET_TRIPLET
)
    list(
        APPEND
        _LUAJIT_INCLUDE_DIRS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/luajit"
    )
    list(
        APPEND
        _LUAJIT_LIBRARY_DIRS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin"
    )
endif()

find_path(
    LUAJIT_INCLUDE_DIR
    NAMES lua.h lualib.h
    HINTS ENV
          LuajitPath${_LIB_SUFFIX}
          ENV
          LuajitPath
          ENV
          DepsPath${_LIB_SUFFIX}
          ENV
          DepsPath
          ${LuajitPath${_LIB_SUFFIX}}
          ${LuajitPath}
          ${DepsPath${_LIB_SUFFIX}}
          ${DepsPath}
          ${_LUAJIT_INCLUDE_DIRS}
    PATHS /usr/include
          /usr/local/include
          /opt/local/include
          /opt/local
          /sw/include
          ~/Library/Frameworks
          /Library/Frameworks
    PATH_SUFFIXES include
                  luajit
                  luajit/src
                  include/luajit
                  include/luajit/src
                  luajit-2.0
                  include/luajit-2.0
                  luajit2.0
                  include/luajit2.0
                  luajit-2.1
                  include/luajit-2.1
                  luajit2.1
                  include/luajit2.1
)

find_library(
    LUAJIT_LIB
    NAMES ${_LUAJIT_LIBRARIES}
          luajit
          luajit-51
          luajit-5.1
          lua51
    HINTS ENV
          LuajitPath${_LIB_SUFFIX}
          ENV
          LuajitPath
          ENV
          DepsPath${_LIB_SUFFIX}
          ENV
          DepsPath
          ${LuajitPath${_LIB_SUFFIX}}
          ${LuajitPath}
          ${DepsPath${_LIB_SUFFIX}}
          ${DepsPath}
          ${_LUAJIT_LIBRARY_DIRS}
    PATHS /usr/lib
          /usr/local/lib
          /opt/local/lib
          /opt/local
          /sw/lib
          ~/Library/Frameworks
          /Library/Frameworks
    PATH_SUFFIXES lib${_LIB_SUFFIX}
                  lib
                  libs${_LIB_SUFFIX}
                  libs
                  bin${_LIB_SUFFIX}
                  bin
                  ../lib${_LIB_SUFFIX}
                  ../lib
                  ../libs${_LIB_SUFFIX}
                  ../libs
                  ../bin${_LIB_SUFFIX}
                  ../bin
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    LuaJIT
    DEFAULT_MSG
    LUAJIT_LIB
    LUAJIT_INCLUDE_DIR
)
mark_as_advanced(LUAJIT_INCLUDE_DIR LUAJIT_LIB)

if(LUAJIT_FOUND)
    if(NOT IS_ABSOLUTE "${LUAJIT_INCLUDE_DIR}")
        get_filename_component(
            LUAJIT_INCLUDE_DIR
            "${LUAJIT_INCLUDE_DIR}"
            ABSOLUTE
            BASE_DIR "${CMAKE_BINARY_DIR}"
        )
    endif()

    if(NOT IS_ABSOLUTE "${LUAJIT_LIB}")
        get_filename_component(
            LUAJIT_LIB
            "${LUAJIT_LIB}"
            ABSOLUTE
            BASE_DIR "${CMAKE_BINARY_DIR}"
        )
    endif()

    set(LUAJIT_INCLUDE_DIRS
        ${LUAJIT_INCLUDE_DIR}
    )
    set(LUAJIT_LIBRARIES
        ${LUAJIT_LIB}
    )
endif()
