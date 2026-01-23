#include "string_util.h"

#include <cctype>
#include <stdexcept>

namespace util {

std::string SanitizeUuid(const std::string& uuid_input) {
    if (uuid_input.size() == 36) {
        const size_t dash_positions[] = {8, 13, 18, 23};
        for (size_t i = 0; i < uuid_input.size(); ++i) {
            const bool is_dash_pos = (i == dash_positions[0] || i == dash_positions[1] ||
                                      i == dash_positions[2] || i == dash_positions[3]);
            if (is_dash_pos) {
                if (uuid_input[i] != '-') {
                    throw std::invalid_argument("UUID has invalid dash positions.");
                }
            } else if (!std::isxdigit(static_cast<unsigned char>(uuid_input[i]))) {
                throw std::invalid_argument("UUID contains non-hex characters.");
            }
        }
        return uuid_input;
    }

    if (uuid_input.size() != 32) {
        throw std::invalid_argument("UUID must be 32 hex characters without dashes.");
    }

    for (char c : uuid_input) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            throw std::invalid_argument("UUID contains non-hex characters.");
        }
    }

    std::string result;
    result.reserve(36);
    for (size_t i = 0; i < uuid_input.size(); ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            result.push_back('-');
        }
        result.push_back(uuid_input[i]);
    }

    return result;
}

}  // namespace util
