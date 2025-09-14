# Find prometheus-cpp via CMake config package
# This expects prometheus-cpp to be installed with CMake package config files.
# It provides imported targets: prometheus-cpp::core and prometheus-cpp::pull
find_package(prometheus-cpp CONFIG REQUIRED)
message(STATUS "Using prometheus-cpp")
