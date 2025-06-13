//
// Created by Ying Han Hung on 2025/6/11.
//
#include "msvc.h"
#include <cctype>
#include <cstring>
#include <strings.h>

int stricmp(const char* str1, const char* str2) {
    // Add null pointer safety to match Visual C++ behavior
    if (str1 == nullptr && str2 == nullptr) {
        return 0;
    }
    if (str1 == nullptr) {
        return -1;
    }
    if (str2 == nullptr) {
        return 1;
    }
    
    // Use standard strcasecmp for the actual comparison
    return strcasecmp(str1, str2);
}

char * strset(char *str, int c) {
    if (str == nullptr) {
        return nullptr;
    }

    char* original = str;
    char fill_char = static_cast<char>(c);

    // while (*str)
    while (*str != '\0') {
        // Modify the memory content of str to fill_char
        *str = fill_char;
        ++str;
    }

    return original;
}

// Implementation of strupr for Linux
char* strupr(char* str) {
    if (!str) return nullptr;
    char* orig = str;
    while (*str) {
        *str = std::toupper(static_cast<unsigned char>(*str));
        ++str;
    }
    return orig;
}