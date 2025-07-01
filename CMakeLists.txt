cmake_minimum_required(VERSION 3.31)
project(CppGrpcDb2)

set(CMAKE_CXX_STANDARD 20)

# Resolve DB2 CLI Driver
include(cmake/db2.cmake)

# Resolve gRPC library
include(cmake/grpc.cmake)

# Get proto files
file(GLOB PROTO_FILES "${CMAKE_SOURCE_DIR}/protos/*.proto")
set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/gen_proto")
file(MAKE_DIRECTORY ${generated_dir})

# Generate protobuf files
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
            COMMENT "Generating gRPC and Protobuf files for ${proto}"
    )

    add_library(${proto_name}_proto
            ${grpc_srcs}
            ${grpc_hdrs}
            ${proto_srcs}
            ${proto_hdrs})
    target_link_libraries(${proto_name}_proto
            ${_REFLECTION}
            ${_GRPC_GRPCPP})

    list(APPEND PROTO_LIBS ${proto_name}_proto)
endforeach()

include_directories(${generated_dir})

foreach (_target greeter_callback_server greeter_server greeter_client)
    add_executable(${_target}
            src/${_target}.cpp
    )

    target_include_directories(${_target}
            PUBLIC ${PROJECT_SOURCE_DIR}/include
            PUBLIC ${generated_dir}
            PUBLIC ${DB2_INCLUDE_DIR}
    )

    # db2 means libdb2, cmake will resolve the naming convention
    target_link_libraries(${_target}
            ${PROTO_LIBS}
            ${_REFLECTION}
            ${_GRPC_GRPCPP}
            ${_PROTOBUF_LIBPROTOBUF}
            db2
    )
endforeach ()

# Enable testing
enable_testing()
include(cmake/gtest.cmake)

# Create library
add_library(msvc STATIC src/msvc/msvc.cpp)

# Create test executable
foreach (_test_target test_strset test_strupr test_stricmp test_max)
    add_executable(${_test_target} tests/msvc/${_test_target}.cpp)
    target_include_directories(${_test_target} PUBLIC src/msvc)
    target_link_libraries(${_test_target} GTest::gtest GTest::gtest_main msvc)
    add_test(NAME ${_test_target} COMMAND ${_test_target})
endforeach ()

# Add DB2 environment variables to the build
#add_custom_target(set_db2_env
#    COMMAND ${CMAKE_COMMAND} -E env
#    "DB2INSTANCE=db2inst1"
#    "DB2INSTDEF=db2inst1"
#    "DB2PATH=${DB2_CLI_DRIVER_DIR}"
#    "DB2HOME=${DB2_CLI_DRIVER_DIR}"
#    "DB2DIR=${DB2_CLI_DRIVER_DIR}"
#    "LD_LIBRARY_PATH=${DB2_LIB_DIR}:$ENV{LD_LIBRARY_PATH}"
#    "PATH=${DB2_CLI_DRIVER_DIR}/bin:$ENV{PATH}"
#    COMMENT "Setting up DB2 environment variables"
#)