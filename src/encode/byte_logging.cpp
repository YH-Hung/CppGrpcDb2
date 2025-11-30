#include "byte_logging.h"

#include <sstream>
#include <iomanip>

#include "spdlog/spdlog.h"

namespace util {

std::string ToHexSpaceDelimited(std::string_view bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        oss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(bytes[i]));
        if (i + 1 < bytes.size()) oss << ' ';
    }
    return oss.str();
}

void LogBytesHexSpaceDelimited(std::string_view bytes, std::string_view label) {
    spdlog::info("{}: {}", label, ToHexSpaceDelimited(bytes));
}

}  // namespace util
