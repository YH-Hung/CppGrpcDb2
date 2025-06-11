cmake_minimum_required(VERSION 3.31)
project(CppGrpcDb2)

set(CMAKE_CXX_STANDARD 20)

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

foreach (_target greeter_server greeter_client)
    add_executable(${_target}
            src/${_target}.cpp
    )

    target_include_directories(${_target}
            PUBLIC ${PROJECT_SOURCE_DIR}/include
            PUBLIC ${generated_dir}
    )

    target_link_libraries(${_target}
            ${PROTO_LIBS}
            ${_REFLECTION}
            ${_GRPC_GRPCPP}
            ${_PROTOBUF_LIBPROTOBUF}
    )
endforeach ()

# Enable testing
enable_testing()
include(cmake/gtest.cmake)

# Create library
add_library(msvc STATIC src/msvc/msvc.cpp)

# Create test executable
add_executable(strset_tests
        tests/msvc/test_strset.cpp
)
target_include_directories(strset_tests PUBLIC 
    src/msvc
)
target_link_libraries(strset_tests GTest::gtest GTest::gtest_main msvc)

add_test(NAME StrsetUnitTests COMMAND strset_tests)