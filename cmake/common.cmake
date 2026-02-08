# Common CMake helper functions for CppGrpcDb2 project

# Function to create a gRPC service executable with common configuration
function(create_grpc_executable target_name source_file)
    add_executable(${target_name} ${source_file})

    target_include_directories(${target_name}
        PRIVATE ${PROJECT_SOURCE_DIR}/include
        PRIVATE ${generated_dir}
    )

    target_link_libraries(${target_name}
        PRIVATE ${PROTO_LIBS}
        PRIVATE ${_REFLECTION}
        PRIVATE ${_GRPC_GRPCPP}
        PRIVATE ${_PROTOBUF_LIBPROTOBUF}
        PRIVATE spdlog::spdlog_header_only
        PRIVATE prometheus-cpp::pull
    )

    # Set C++20 standard
    target_compile_features(${target_name} PRIVATE cxx_std_20)
endfunction()

# Function to add DB2 support to a target
function(add_db2_support target_name)
    # Link against our DB2 wrapper (which itself links DB2::db2)
    target_link_libraries(${target_name}
        PRIVATE db2_wrapper
    )
endfunction()

# Function to add interceptor support to a target
function(add_interceptor_support target_name)
    cmake_parse_arguments(ARG "STRING_TRANSFORM;METRICS" "" "" ${ARGN})

    if(ARG_STRING_TRANSFORM)
        target_link_libraries(${target_name}
            PRIVATE string_transform_interceptor
        )
    endif()

    if(ARG_METRICS)
        target_link_libraries(${target_name}
            PRIVATE metrics_interceptor
        )
    endif()
endfunction()

# Function to create a test executable with common configuration
function(create_test_executable target_name source_file)
    add_executable(${target_name} ${source_file})

    target_include_directories(${target_name}
        PRIVATE src/msvc
    )

    target_link_libraries(${target_name}
        PRIVATE GTest::gtest
        PRIVATE GTest::gtest_main
        PRIVATE msvc
    )

    add_test(NAME ${target_name} COMMAND ${target_name})

    # Set C++20 standard
    target_compile_features(${target_name} PRIVATE cxx_std_20)
endfunction()