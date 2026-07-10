# Resolve the Halcyon Db2 client (installed under $HOME/.local).
# find_package transitively imports DB2::CLI via Halcyon's bundled
# FindDB2CLI.cmake, whose default DB2_CLIDRIVER_ROOT is
# ${CMAKE_SOURCE_DIR}/third_party/clidriver — exactly where this repo
# vendors the driver — so the CLI driver resolves with no extra config.
find_package(Halcyon REQUIRED)
message(STATUS "Halcyon found: target halcyon::halcyon")
