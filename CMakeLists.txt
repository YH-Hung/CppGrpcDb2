cmake_minimum_required(VERSION 3.31)
project(CppGrpcDb2 VERSION 1.0.0 LANGUAGES CXX)

# Set global C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# Dependency Resolution
# ==============================================================================

# Resolve DB2 CLI Driver
include(cmake/db2.cmake)

# Resolve gRPC library
include(cmake/grpc.cmake)

# Resolve spdlog library
include(cmake/spdlog.cmake)

# Resolve prometheus library
include(cmake/prometheus.cmake)

# Resolve utf8ansi library (installed under $HOME/.local by default)
include(cmake/utf8ansi.cmake)

# Include common helper functions
include(cmake/common.cmake)

# Validate required directories exist
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src")
    message(FATAL_ERROR "Source directory not found: ${CMAKE_SOURCE_DIR}/src")
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/include")
    message(FATAL_ERROR "Include directory not found: ${CMAKE_SOURCE_DIR}/include")
endif()

# Make headers under src/ available to all targets (for header-only utilities like resource pool)
include_directories(${PROJECT_SOURCE_DIR}/src)

# ==============================================================================
# Protobuf and gRPC Code Generation
# ==============================================================================

# Get proto files and validate they exist
file(GLOB PROTO_FILES "${CMAKE_SOURCE_DIR}/protos/*.proto")
if(NOT PROTO_FILES)
    message(FATAL_ERROR "No .proto files found in ${CMAKE_SOURCE_DIR}/protos/")
endif()

set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/gen_proto")
file(MAKE_DIRECTORY "${generated_dir}")

# Generate protobuf files for each proto file
foreach(proto ${PROTO_FILES})
    get_filename_component(proto_name ${proto} NAME_WE)
    get_filename_component(proto_path ${proto} PATH)

    set(proto_srcs "${generated_dir}/${proto_name}.pb.cc")
    set(proto_hdrs "${generated_dir}/${proto_name}.pb.h")
    set(grpc_srcs "${generated_dir}/${proto_name}.grpc.pb.cc")
    set(grpc_hdrs "${generated_dir}/${proto_name}.grpc.pb.h")

    add_custom_command(
        OUTPUT ${proto_srcs} ${proto_hdrs} ${grpc_srcs} ${grpc_hdrs}
        COMMAND ${_PROTOBUF_PROTOC}
        ARGS --grpc_out=${generated_dir}
             --cpp_out=${generated_dir}
             -I${proto_path}
             --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN_EXECUTABLE}
             ${proto}
        DEPENDS ${proto}
        COMMENT "Generating gRPC and Protobuf files for ${proto_name}"
    )

    # Create proto library with modern target properties
    add_library(${proto_name}_proto
        ${grpc_srcs}
        ${grpc_hdrs}
        ${proto_srcs}
        ${proto_hdrs}
    )

    target_link_libraries(${proto_name}_proto
        PUBLIC ${_REFLECTION}
        PUBLIC ${_GRPC_GRPCPP}
    )

    target_include_directories(${proto_name}_proto
        PUBLIC "${generated_dir}"
    )

    # Set C++20 standard for proto libraries
    target_compile_features(${proto_name}_proto PUBLIC cxx_std_20)

    list(APPEND PROTO_LIBS ${proto_name}_proto)
endforeach()

# ==============================================================================
# Interceptor Libraries
# ==============================================================================

# String transform interceptor library
add_library(string_transform_interceptor
        src/interceptor/string_transform_interceptor.cpp
)

target_include_directories(string_transform_interceptor
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PUBLIC "${generated_dir}"
)

target_link_libraries(string_transform_interceptor
    PUBLIC ${_PROTOBUF_LIBPROTOBUF}
    PUBLIC spdlog::spdlog
    PUBLIC utf8ansi::utf8ansi
)

target_compile_features(string_transform_interceptor PUBLIC cxx_std_20)

# Metrics interceptor library
add_library(metrics_interceptor
        src/interceptor/metrics_interceptor.cpp
)

target_include_directories(metrics_interceptor
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PUBLIC "${generated_dir}"
)

target_link_libraries(metrics_interceptor
    PUBLIC prometheus-cpp::pull
    PUBLIC ${_GRPC_GRPCPP}
)

target_compile_features(metrics_interceptor PUBLIC cxx_std_20)

# Message logging interceptor library
add_library(message_logging_interceptor
        src/interceptor/message_logging_interceptor.cpp
)

target_include_directories(message_logging_interceptor
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PUBLIC "${generated_dir}"
)

target_link_libraries(message_logging_interceptor
    PUBLIC ${_PROTOBUF_LIBPROTOBUF}
    PUBLIC spdlog::spdlog
    PUBLIC ${_GRPC_GRPCPP}
)

target_compile_features(message_logging_interceptor PUBLIC cxx_std_20)

# CallData metrics library
add_library(calldata_metrics
        src/metrics/calldata_metrics.cpp
)

target_include_directories(calldata_metrics
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PUBLIC "${generated_dir}"
)

target_link_libraries(calldata_metrics
    PUBLIC prometheus-cpp::pull
    PUBLIC ${_GRPC_GRPCPP}
)

target_compile_features(calldata_metrics PUBLIC cxx_std_20)

# ==============================================================================
# DB2 Wrapper Library
# ==============================================================================

add_library(db2_wrapper
    src/db2/db2.cpp
        src/util/sql_util.cpp
        src/util/sql_util.h
)

target_include_directories(db2_wrapper
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(db2_wrapper
    PUBLIC DB2::db2
)

target_compile_features(db2_wrapper PUBLIC cxx_std_20)

# ==============================================================================
# Main Executable Targets
# ==============================================================================

# Define executable targets and their specific configurations
set(GRPC_TARGETS
    greeter_callback_server
    greeter_server
    greeter_client
    greeter_girl_client
)

foreach(target_name ${GRPC_TARGETS})
    # Create the base gRPC executable
    create_grpc_executable(${target_name} "src/${target_name}.cpp")

    # Add DB2 support to all targets
    add_db2_support(${target_name})

    # Add interceptor support based on target type
    if(target_name STREQUAL "greeter_callback_server")
        add_interceptor_support(${target_name} STRING_TRANSFORM METRICS)
    elseif(target_name STREQUAL "greeter_server")
        add_interceptor_support(${target_name} METRICS)
    endif()
endforeach()

# ==============================================================================
# Test and Utility Executables
# ==============================================================================

# Test for string interceptor (without DB2 dependency)
add_executable(test_string_interceptor
        src/interceptor/test_string_interceptor.cpp
)

target_include_directories(test_string_interceptor
    PRIVATE ${PROJECT_SOURCE_DIR}/include
    PRIVATE "${generated_dir}"
)

target_link_libraries(test_string_interceptor
    PRIVATE ${PROTO_LIBS}
    PRIVATE ${_PROTOBUF_LIBPROTOBUF}
    PRIVATE string_transform_interceptor
)

target_compile_features(test_string_interceptor PRIVATE cxx_std_20)

# Test server without DB2 dependency
add_executable(greeter_callback_server_no_db2
    src/greeter_callback_server_no_db2.cpp
        src/encode/byte_logging.cpp
)

target_include_directories(greeter_callback_server_no_db2
    PRIVATE ${PROJECT_SOURCE_DIR}/include
    PRIVATE "${generated_dir}"
)

target_link_libraries(greeter_callback_server_no_db2
    PRIVATE ${PROTO_LIBS}
    PRIVATE ${_REFLECTION}
    PRIVATE ${_GRPC_GRPCPP}
    PRIVATE ${_PROTOBUF_LIBPROTOBUF}
    PRIVATE spdlog::spdlog
    PRIVATE string_transform_interceptor
    PRIVATE utf8ansi::utf8ansi
)

target_compile_features(greeter_callback_server_no_db2 PRIVATE cxx_std_20)

# ==============================================================================
# Async Server
# ==============================================================================

add_executable(complex_proto_async
        src/call_data/CallData.h
        src/call_data/GreeterSayHelloCallData.cpp
        src/call_data/GreeterSayHelloCallData.h
        src/call_data/HelloGirlSayHelloCallData.cpp
        src/call_data/HelloGirlSayHelloCallData.h
        src/complex_async_server_single_cq.cpp
        src/encode/byte_logging.cpp
)

target_include_directories(complex_proto_async
        PRIVATE ${PROJECT_SOURCE_DIR}/include
        PRIVATE "${generated_dir}"
)

target_link_libraries(complex_proto_async
        PRIVATE ${PROTO_LIBS}
        PRIVATE ${_REFLECTION}
        PRIVATE ${_GRPC_GRPCPP}
        PRIVATE ${_PROTOBUF_LIBPROTOBUF}
        PRIVATE spdlog::spdlog
        PRIVATE message_logging_interceptor
        PRIVATE calldata_metrics
        PRIVATE prometheus-cpp::pull
)

# Ensure health_proto is linked (it should be in PROTO_LIBS if health.proto exists)
# Explicitly check and add if needed
#if(TARGET health_proto)
#    target_link_libraries(complex_proto_async PRIVATE health_proto)
#endif()

# ==============================================================================
# Unit Tests
# ==============================================================================

# Enable testing framework
enable_testing()
include(cmake/gtest.cmake)

# MSVC compatibility library
add_library(msvc STATIC src/msvc/msvc.cpp)
target_include_directories(msvc
    PUBLIC src/msvc
)
target_compile_features(msvc PUBLIC cxx_std_20)

# MSVC library test targets
set(MSVC_TEST_TARGETS
    test_strset
    test_strupr
    test_stricmp
    test_max
)

foreach(test_target ${MSVC_TEST_TARGETS})
    create_test_executable(${test_target} "tests/msvc/${test_target}.cpp")
endforeach()

# WorkerPool unit tests
create_test_executable(worker_pool_tests "tests/worker/test_worker_pool.cpp")

# SqlUtil unit tests
add_executable(test_sql_util tests/util/test_sql_util.cpp src/util/sql_util.cpp)
target_include_directories(test_sql_util PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_sql_util PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME test_sql_util COMMAND test_sql_util)
target_compile_features(test_sql_util PRIVATE cxx_std_20)

# DB2 wrapper integration tests (opt-in, require DB2 client runtime to be present)
option(BUILD_DB2_TESTS "Build DB2 wrapper integration tests" OFF)
if(BUILD_DB2_TESTS)
    add_executable(db2_wrapper_tests
        tests/db2/test_db2_wrapper.cpp
    )

    target_include_directories(db2_wrapper_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(db2_wrapper_tests
        PRIVATE GTest::gtest
        PRIVATE GTest::gtest_main
        PRIVATE db2_wrapper
    )

    add_test(NAME db2_wrapper_tests COMMAND db2_wrapper_tests)

    target_compile_features(db2_wrapper_tests PRIVATE cxx_std_20)

    # Resource pool refactoring tests
    add_executable(resource_handle_refactor_tests
        tests/resource/test_resource_handle_refactor.cpp
    )

    target_include_directories(resource_handle_refactor_tests
        PRIVATE ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(resource_handle_refactor_tests
        PRIVATE GTest::gtest
        PRIVATE GTest::gtest_main
    )

    add_test(NAME resource_handle_refactor_tests COMMAND resource_handle_refactor_tests)

    target_compile_features(resource_handle_refactor_tests PRIVATE cxx_std_20)
endif()
