#ifndef CPPGRPCDB2_STRING_UTIL_H
#define CPPGRPCDB2_STRING_UTIL_H

#include <string>

namespace util {

// Accept UUID with or without dashes; return dashed UUID or original dashed input.
// Throws std::invalid_argument on invalid input.
std::string SanitizeUuid(const std::string& uuid_input);

}  // namespace util

#endif //CPPGRPCDB2_STRING_UTIL_H
