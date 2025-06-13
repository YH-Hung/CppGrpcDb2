# Resolve CLI driver location
if (DB2_CLI_INSTALL_PREFIX)
    set(DB2_CLI_DRIVER_DIR "${DB2_CLI_INSTALL_PREFIX}/clidriver")
    message(STATUS "Ues pre-located DB2 CLI driver")
else ()
    set(DB2_CLI_DRIVER_DIR "${CMAKE_SOURCE_DIR}/third_party/clidriver")
    message(STATUS "DB2 CLI driver as submodule")
endif ()

set(DB2_INCLUDE_DIR "${DB2_CLI_DRIVER_DIR}/include")
set(DB2_LIB_DIR "${DB2_CLI_DRIVER_DIR}/lib")

# Add DB2 include directories
include_directories(${DB2_INCLUDE_DIR})

# Link DB2 libraries
link_directories(${DB2_LIB_DIR})