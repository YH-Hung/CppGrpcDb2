# Resolve CLI driver location
if(DB2_CLI_INSTALL_PREFIX)
    set(DB2_CLI_DRIVER_DIR "${DB2_CLI_INSTALL_PREFIX}/clidriver")
    message(STATUS "Use pre-located DB2 CLI driver")
else()
    set(DB2_CLI_DRIVER_DIR "${CMAKE_SOURCE_DIR}/third_party/clidriver")
    message(STATUS "DB2 CLI driver as submodule")
endif()

# Validate DB2 CLI driver directory exists
if(NOT EXISTS "${DB2_CLI_DRIVER_DIR}")
    message(FATAL_ERROR "DB2 CLI driver directory not found: ${DB2_CLI_DRIVER_DIR}")
endif()

set(DB2_INCLUDE_DIR "${DB2_CLI_DRIVER_DIR}/include")
set(DB2_LIB_DIR "${DB2_CLI_DRIVER_DIR}/lib")

# Find the DB2 library
find_library(DB2_LIBRARY
    NAMES db2 libdb2
    PATHS "${DB2_LIB_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

# Create imported target for DB2
if(NOT TARGET DB2::db2)
    add_library(DB2::db2 SHARED IMPORTED)
    set_target_properties(DB2::db2 PROPERTIES
        IMPORTED_LOCATION "${DB2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DB2_INCLUDE_DIR}"
    )
endif()