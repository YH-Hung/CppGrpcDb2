#ifndef CPPGRPCDB2_STRING_UTIL_H
#define CPPGRPCDB2_STRING_UTIL_H

#include <cstddef>
#include <string>

namespace util {

// Accept UUID with or without dashes; return dashed UUID or original dashed input.
// Throws std::invalid_argument on invalid input.
std::string SanitizeUuid(const std::string& uuid_input);

// Copies string bytes into output buffer, truncating if needed.
// Returns number of bytes copied (excluding null terminator).
size_t CopyStringToBuffer(char* output, const std::string& input, size_t output_size);

}  // namespace util

#endif //CPPGRPCDB2_STRING_UTIL_H
