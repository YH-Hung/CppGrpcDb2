#pragma once

#include <string>
#include <string_view>

namespace util {

// Logs the provided bytes as a space-delimited lowercase hexadecimal string
// with the given label prefix, e.g.:
//   label: "Name bytes (hex)" => "Name bytes (hex): 41 42 0a"
// Uses spdlog::info under the hood.
void LogBytesHexSpaceDelimited(std::string_view bytes, std::string_view label);

// Helper that returns the space-delimited lowercase hex string for the given bytes.
// Example: input "AB\n" => "41 42 0a".
std::string ToHexSpaceDelimited(std::string_view bytes);

}  // namespace util
