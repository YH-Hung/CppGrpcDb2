#include <gtest/gtest.h>
#include "msvc.h"

class StricmpTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test case: Equal strings (same case)
TEST_F(StricmpTest, EqualStringsSameCase) {
    EXPECT_EQ(stricmp("hello", "hello"), 0);
    EXPECT_EQ(stricmp("", ""), 0);
    EXPECT_EQ(stricmp("test123", "test123"), 0);
}

// Test case: Equal strings (different case)
TEST_F(StricmpTest, EqualStringsDifferentCase) {
    EXPECT_EQ(stricmp("hello", "HELLO"), 0);
    EXPECT_EQ(stricmp("HELLO", "hello"), 0);
    EXPECT_EQ(stricmp("Hello", "hELLo"), 0);
    EXPECT_EQ(stricmp("TeSt123", "test123"), 0);
    EXPECT_EQ(stricmp("MiXeD", "mixed"), 0);
}

// Test case: First string is lexicographically smaller
TEST_F(StricmpTest, FirstStringSmallerIgnoreCase) {
    EXPECT_LT(stricmp("abc", "def"), 0);
    EXPECT_LT(stricmp("ABC", "def"), 0);
    EXPECT_LT(stricmp("abc", "DEF"), 0);
    EXPECT_LT(stricmp("hello", "world"), 0);
    EXPECT_LT(stricmp("APPLE", "banana"), 0);
}

// Test case: First string is lexicographically larger
TEST_F(StricmpTest, FirstStringLargerIgnoreCase) {
    EXPECT_GT(stricmp("def", "abc"), 0);
    EXPECT_GT(stricmp("DEF", "abc"), 0);
    EXPECT_GT(stricmp("def", "ABC"), 0);
    EXPECT_GT(stricmp("world", "hello"), 0);
    EXPECT_GT(stricmp("ZEBRA", "apple"), 0);
}

// Test case: One string is prefix of another
TEST_F(StricmpTest, PrefixComparison) {
    EXPECT_LT(stricmp("hello", "hello world"), 0);
    EXPECT_LT(stricmp("HELLO", "hello world"), 0);
    EXPECT_LT(stricmp("hello", "HELLO WORLD"), 0);
    EXPECT_GT(stricmp("hello world", "hello"), 0);
    EXPECT_GT(stricmp("HELLO WORLD", "hello"), 0);
    EXPECT_GT(stricmp("hello world", "HELLO"), 0);
}

// Test case: Empty strings
TEST_F(StricmpTest, EmptyStrings) {
    EXPECT_EQ(stricmp("", ""), 0);
    EXPECT_LT(stricmp("", "hello"), 0);
    EXPECT_GT(stricmp("hello", ""), 0);
    EXPECT_LT(stricmp("", "HELLO"), 0);
    EXPECT_GT(stricmp("HELLO", ""), 0);
}

// Test case: Null pointer handling
TEST_F(StricmpTest, NullPointerHandling) {
    EXPECT_EQ(stricmp(nullptr, nullptr), 0);
    EXPECT_LT(stricmp(nullptr, "hello"), 0);
    EXPECT_GT(stricmp("hello", nullptr), 0);
    EXPECT_LT(stricmp(nullptr, ""), 0);
    EXPECT_GT(stricmp("", nullptr), 0);
}

// Test case: Special characters and numbers
TEST_F(StricmpTest, SpecialCharactersAndNumbers) {
    EXPECT_EQ(stricmp("test123", "TEST123"), 0);
    EXPECT_EQ(stricmp("hello@world.com", "HELLO@WORLD.COM"), 0);
    EXPECT_LT(stricmp("test123", "test124"), 0);
    EXPECT_GT(stricmp("TEST124", "test123"), 0);
    EXPECT_EQ(stricmp("!@#$%", "!@#$%"), 0);
}

// Test case: Unicode/Extended ASCII behavior
// TEST_F(StricmpTest, ExtendedAscii) {
//     // Test with characters that have different upper/lower case behavior
//     EXPECT_EQ(stricmp("café", "CAFÉ"), 0);  // This might fail depending on locale
//     EXPECT_EQ(stricmp("naïve", "NAÏVE"), 0);  // This might fail depending on locale
// }

// Test case: Long strings
TEST_F(StricmpTest, LongStrings) {
    std::string long_str1(1000, 'a');
    std::string long_str2(1000, 'A');
    std::string long_str3 = long_str1 + "b";
    std::string long_str4 = long_str2 + "B";
    
    EXPECT_EQ(stricmp(long_str1.c_str(), long_str2.c_str()), 0);
    EXPECT_EQ(stricmp(long_str3.c_str(), long_str4.c_str()), 0);
    EXPECT_LT(stricmp(long_str1.c_str(), long_str3.c_str()), 0);
    EXPECT_GT(stricmp(long_str3.c_str(), long_str1.c_str()), 0);
}

// Test case: Single character comparisons
TEST_F(StricmpTest, SingleCharacter) {
    EXPECT_EQ(stricmp("a", "A"), 0);
    EXPECT_EQ(stricmp("Z", "z"), 0);
    EXPECT_LT(stricmp("a", "B"), 0);
    EXPECT_GT(stricmp("Z", "a"), 0);
    EXPECT_LT(stricmp("A", "b"), 0);
    EXPECT_GT(stricmp("z", "A"), 0);
}

// Performance test (optional)
TEST_F(StricmpTest, PerformanceTest) {
    const char* str1 = "This is a moderately long string for performance testing";
    const char* str2 = "THIS IS A MODERATELY LONG STRING FOR PERFORMANCE TESTING";
    
    // Run comparison multiple times to test performance
    for (int i = 0; i < 10000; ++i) {
        EXPECT_EQ(stricmp(str1, str2), 0);
    }
}