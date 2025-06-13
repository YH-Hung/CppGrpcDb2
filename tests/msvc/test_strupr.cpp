#include <gtest/gtest.h>
#include "msvc.h"
#include <cstring>

TEST(StruprTest, NullptrInput) {
    EXPECT_EQ(strupr(nullptr), nullptr);
}

TEST(StruprTest, EmptyString) {
    char data[] = "";
    EXPECT_STREQ(strupr(data), "");
}

TEST(StruprTest, LowercaseOnly) {
    char data[] = "abcdef";
    EXPECT_STREQ(strupr(data), "ABCDEF");
}

TEST(StruprTest, MixedCase) {
    char data[] = "aBcDeF";
    EXPECT_STREQ(strupr(data), "ABCDEF");
}

TEST(StruprTest, WithNumbersAndSymbols) {
    char data[] = "123abc!@#";
    EXPECT_STREQ(strupr(data), "123ABC!@#");
}

TEST(StruprTest, AlreadyUppercase) {
    char data[] = "ABCDEF";
    EXPECT_STREQ(strupr(data), "ABCDEF");
}