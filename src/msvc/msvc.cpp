//
// Created by Ying Han Hung on 2025/6/11.
//
#include "msvc.h"

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
