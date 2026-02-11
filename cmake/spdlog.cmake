# https://github.com/gabime/spdlog/blob/v1.x/example/CMakeLists.txt
find_package(spdlog REQUIRED)

# Homebrew upgrades can leave spdlog built against an older fmt ABI; header-only avoids that mismatch.
option(USE_SPDLOG_HEADER_ONLY "Use spdlog header-only target" ON)
if(USE_SPDLOG_HEADER_ONLY)
    set(SPDLOG_TARGET spdlog::spdlog_header_only)
else()
    set(SPDLOG_TARGET spdlog::spdlog)
endif()
