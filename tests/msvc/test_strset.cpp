#include <gtest/gtest.h>
#include "msvc.h"
#include <cstring>

class StrsetTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code for each test
    }

    void TearDown() override {
        // Cleanup code for each test
    }
};

TEST_F(StrsetTest, BasicFunctionality) {
    char test_str[] = "hello";
    char* result = strset(test_str, 'x');

    EXPECT_STREQ(test_str, "xxxxx");
    EXPECT_EQ(result, test_str);
}

TEST_F(StrsetTest, SingleCharacter) {
    char test_str[] = "a";
    strset(test_str, 'z');

    EXPECT_STREQ(test_str, "z");
}

TEST_F(StrsetTest, EmptyString) {
    char test_str[] = "";
    char* result = strset(test_str, 'x');

    EXPECT_STREQ(test_str, "");
    EXPECT_EQ(result, test_str);
}

TEST_F(StrsetTest, NullPointer) {
    char* result = strset(nullptr, 'x');
    EXPECT_EQ(result, nullptr);
}

TEST_F(StrsetTest, SpecialCharacters) {
    char test_str[] = "test";
    strset(test_str, '@');

    EXPECT_STREQ(test_str, "@@@@");
}

TEST_F(StrsetTest, NumericCharacter) {
    char test_str[] = "abc";
    strset(test_str, '1');

    EXPECT_STREQ(test_str, "111");
}

TEST_F(StrsetTest, ZeroCharacter) {
    char test_str[] = "hello";
    strset(test_str, '\0');

    EXPECT_EQ(test_str[0], '\0');
    EXPECT_EQ(strlen(test_str), 0);
}

TEST_F(StrsetTest, LongString) {
    char test_str[100];
    strcpy(test_str, "This is a very long string for testing purposes");
    size_t original_len = strlen(test_str);

    strset(test_str, 'L');

    for (size_t i = 0; i < original_len; ++i) {
        EXPECT_EQ(test_str[i], 'L');
    }
    EXPECT_EQ(strlen(test_str), original_len);
}

TEST_F(StrsetTest, ExtendedASCII) {
    char test_str[] = "test";
    strset(test_str, 200);

    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(static_cast<unsigned char>(test_str[i]), 200);
    }
}

TEST_F(StrsetTest, StringWithSpaces) {
    char test_str[] = "hello world";
    strset(test_str, '-');

    EXPECT_STREQ(test_str, "-----------");
}