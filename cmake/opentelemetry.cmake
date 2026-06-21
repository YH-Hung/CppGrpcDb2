# Find opentelemetry-cpp via CMake config package.
# This expects opentelemetry-cpp to be installed with CMake package config files.
# It provides imported targets such as opentelemetry-cpp::api,
# opentelemetry-cpp::sdk, and opentelemetry-cpp::otlp_grpc_exporter.
#
# The OTLP gRPC exporter ships generated protobuf headers. Those headers must be
# compiled against the same protobuf/gRPC family used when opentelemetry-cpp was
# packaged. Mixed prefixes such as opentelemetry-cpp from Homebrew and protobuf
# from $HOME/.local fail at compile time with protobuf gencode/runtime version
# errors, so select matching package config dirs before importing anything.

find_file(OPENTELEMETRY_CPP_CONFIG_FILE
    NAMES opentelemetry-cpp-config.cmake
    PATH_SUFFIXES lib/cmake/opentelemetry-cpp
)

if(NOT OPENTELEMETRY_CPP_CONFIG_FILE)
    message(FATAL_ERROR "Could not find opentelemetry-cpp-config.cmake")
endif()

get_filename_component(_otel_config_dir "${OPENTELEMETRY_CPP_CONFIG_FILE}" DIRECTORY)
get_filename_component(_otel_prefix "${_otel_config_dir}/../../.." ABSOLUTE)

set(opentelemetry-cpp_DIR "${_otel_config_dir}" CACHE PATH
    "Directory containing opentelemetry-cpp-config.cmake" FORCE)

set(_otel_dependency_config_dirs
    Protobuf "${_otel_prefix}/lib/cmake/protobuf"
    gRPC "${_otel_prefix}/lib/cmake/grpc"
    absl "${_otel_prefix}/lib/cmake/absl"
    re2 "${_otel_prefix}/lib/cmake/re2"
    nlohmann_json "${_otel_prefix}/share/cmake/nlohmann_json"
)

while(_otel_dependency_config_dirs)
    list(POP_FRONT _otel_dependency_config_dirs _pkg _pkg_dir)
    if(EXISTS "${_pkg_dir}")
        set(${_pkg}_DIR "${_pkg_dir}" CACHE PATH
            "Directory containing ${_pkg} CMake config files" FORCE)
    endif()
endwhile()

# gRPC's exported targets can reference OpenSSL imported targets without
# creating them, depending on how the package was built. Homebrew's OpenSSL is
# keg-only, so provide the common prefixes when the caller did not.
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    foreach(_openssl_root "/opt/homebrew/opt/openssl@3" "/usr/local/opt/openssl@3")
        if(EXISTS "${_openssl_root}")
            set(OPENSSL_ROOT_DIR "${_openssl_root}")
            break()
        endif()
    endforeach()
endif()
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)

find_package(opentelemetry-cpp CONFIG REQUIRED
    COMPONENTS api sdk exporters_otlp_grpc
)
message(STATUS "Using opentelemetry-cpp from ${_otel_prefix}")
