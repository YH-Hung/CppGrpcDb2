# Find jsoncpp via CMake config package; fallback to FetchContent if missing.
find_package(JsonCpp CONFIG QUIET)
if(NOT JsonCpp_FOUND)
    find_package(jsoncpp CONFIG QUIET)
endif()

set(JSONCPP_TARGET "")

if(TARGET JsonCpp::JsonCpp)
    set(JSONCPP_TARGET JsonCpp::JsonCpp)
elseif(TARGET jsoncpp_lib)
    set(JSONCPP_TARGET jsoncpp_lib)
elseif(TARGET jsoncpp_static)
    set(JSONCPP_TARGET jsoncpp_static)
endif()

if(NOT JSONCPP_TARGET)
    include(FetchContent)
    FetchContent_Declare(
        jsoncpp
        GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
        GIT_TAG 1.9.5
    )

    set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
    set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(jsoncpp)

    if(TARGET JsonCpp::JsonCpp)
        set(JSONCPP_TARGET JsonCpp::JsonCpp)
    elseif(TARGET jsoncpp_lib)
        set(JSONCPP_TARGET jsoncpp_lib)
    elseif(TARGET jsoncpp_static)
        set(JSONCPP_TARGET jsoncpp_static)
    endif()
endif()

if(NOT JSONCPP_TARGET)
    message(FATAL_ERROR "jsoncpp target not found after configuration.")
endif()

message(STATUS "Using jsoncpp target: ${JSONCPP_TARGET}")
