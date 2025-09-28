# Locate the locally installed utf8ansi library under $HOME/.local and expose an imported target `utf8ansi::utf8ansi`.
#
# Usage in this project:
#   include(cmake/utf8ansi.cmake)
#   # Then link with: target_link_libraries(your_target utf8ansi::utf8ansi)
#
# You can override the default root via:
#   -DUTF8ANSI_ROOT=/custom/prefix
# Or explicitly specify:
#   -DUTF8ANSI_INCLUDE_DIR=/path/include -DUTF8ANSI_LIBRARY=/path/lib/libutf8_ansi_cpp.(a|so|dylib)

# Default installation prefix (user-local)
if(NOT DEFINED UTF8ANSI_ROOT)
    set(UTF8ANSI_ROOT "$ENV{HOME}/.local" CACHE PATH "Root of utf8ansi installation")
endif()

# Allow manual overrides
set(UTF8ANSI_INCLUDE_DIR "${UTF8ANSI_INCLUDE_DIR}" CACHE PATH "utf8ansi include directory override")
set(UTF8ANSI_LIBRARY "${UTF8ANSI_LIBRARY}" CACHE FILEPATH "utf8ansi library file override")

# Candidate include directories and library directories under the prefix
set(_UTF8ANSI_CANDIDATE_INCLUDE_DIRS
    "${UTF8ANSI_ROOT}/include"
    "$ENV{HOME}/.local/include"
)
set(_UTF8ANSI_CANDIDATE_LIB_DIRS
    "${UTF8ANSI_ROOT}/lib"
    "${UTF8ANSI_ROOT}/lib64"
    "$ENV{HOME}/.local/lib"
    "$ENV{HOME}/.local/lib64"
)

# Try to locate header – try a few common header names/paths
if(NOT UTF8ANSI_INCLUDE_DIR)
    find_path(UTF8ANSI_INCLUDE_DIR
        NAMES
            utf8ansi.h
            utf8ansi/utf8ansi.h
            utf8-ansi.h
            utf8/ansi.h
        HINTS ${_UTF8ANSI_CANDIDATE_INCLUDE_DIRS}
    )
    # Fallback to explicit default path if still not found
    if(NOT UTF8ANSI_INCLUDE_DIR AND EXISTS "$ENV{HOME}/.local/include/utf8ansi.h")
        set(UTF8ANSI_INCLUDE_DIR "$ENV{HOME}/.local/include")
    endif()
endif()

# Try to locate the library – try common names
if(NOT UTF8ANSI_LIBRARY)
    find_library(UTF8ANSI_LIBRARY
        NAMES utf8_ansi_cpp utf8ansi utf8-ansi
        HINTS ${_UTF8ANSI_CANDIDATE_LIB_DIRS}
    )
    # Fallback to explicit default path if still not found
    if(NOT UTF8ANSI_LIBRARY AND EXISTS "$ENV{HOME}/.local/lib/libutf8_ansi_cpp.dylib")
        set(UTF8ANSI_LIBRARY "$ENV{HOME}/.local/lib/libutf8_ansi_cpp.dylib")
    endif()
endif()

# Determine if found
include(CheckSymbolExists)
set(UTF8ANSI_FOUND OFF)
if(UTF8ANSI_INCLUDE_DIR AND UTF8ANSI_LIBRARY)
    set(UTF8ANSI_FOUND ON)
endif()

# Create imported target if found
if(UTF8ANSI_FOUND)
    # Locate ICU dependency required by utf8ansi
    if(APPLE AND NOT DEFINED ICU_ROOT)
        if(EXISTS "/opt/homebrew/opt/icu4c")
            set(ICU_ROOT "/opt/homebrew/opt/icu4c" CACHE PATH "ICU root path")
        elseif(EXISTS "/usr/local/opt/icu4c")
            set(ICU_ROOT "/usr/local/opt/icu4c" CACHE PATH "ICU root path")
        endif()
    endif()
    find_package(ICU QUIET COMPONENTS uc i18n data)

    if(NOT ICU_FOUND)
        message(FATAL_ERROR "utf8ansi was found (include at ${UTF8ANSI_INCLUDE_DIR}, lib at ${UTF8ANSI_LIBRARY}) but ICU was not found. Install ICU (e.g., Homebrew: brew install icu4c) or set ICU_ROOT to its prefix (e.g., /opt/homebrew/opt/icu4c).")
    endif()

    if(NOT TARGET utf8ansi::utf8ansi)
        add_library(utf8ansi::utf8ansi UNKNOWN IMPORTED)
        set_target_properties(utf8ansi::utf8ansi PROPERTIES
            IMPORTED_LOCATION "${UTF8ANSI_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${UTF8ANSI_INCLUDE_DIR};${ICU_INCLUDE_DIRS}"
        )
        # Propagate ICU libraries to dependents
        set_property(TARGET utf8ansi::utf8ansi APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${ICU_LIBRARIES}")
    endif()
    message(STATUS "utf8ansi: using include ${UTF8ANSI_INCLUDE_DIR}, lib ${UTF8ANSI_LIBRARY}; ICU include ${ICU_INCLUDE_DIRS}, libs ${ICU_LIBRARIES}")
else()
    # Do not error to keep this optional unless explicitly requested by user.
    message(STATUS "utf8ansi: not found under ${UTF8ANSI_ROOT}. Set UTF8ANSI_ROOT or UTF8ANSI_INCLUDE_DIR/UTF8ANSI_LIBRARY if installed elsewhere.")
endif()

mark_as_advanced(UTF8ANSI_ROOT UTF8ANSI_INCLUDE_DIR UTF8ANSI_LIBRARY)
