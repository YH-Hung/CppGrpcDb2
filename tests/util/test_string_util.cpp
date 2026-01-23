#include <gtest/gtest.h>

#include "util/string_util.h"

TEST(StringUtilTest, ValidLowercaseUuid) {
    std::string input = "123e4567e89b12d3a456426614174000";
    std::string expected = "123e4567-e89b-12d3-a456-426614174000";
    EXPECT_EQ(util::SanitizeUuid(input), expected);
}

TEST(StringUtilTest, ValidUppercaseUuid) {
    std::string input = "123E4567E89B12D3A456426614174000";
    std::string expected = "123E4567-E89B-12D3-A456-426614174000";
    EXPECT_EQ(util::SanitizeUuid(input), expected);
}

TEST(StringUtilTest, ValidMixedCaseUuid) {
    std::string input = "123e4567E89b12D3a456426614174000";
    std::string expected = "123e4567-E89b-12D3-a456-426614174000";
    EXPECT_EQ(util::SanitizeUuid(input), expected);
}

TEST(StringUtilTest, ValidAllZeroUuid) {
    std::string input = "00000000000000000000000000000000";
    std::string expected = "00000000-0000-0000-0000-000000000000";
    EXPECT_EQ(util::SanitizeUuid(input), expected);
}

TEST(StringUtilTest, ValidAdditionalUuids) {
    struct Case {
        const char* input;
        const char* expected;
    };

    const Case cases[] = {
        {"ffffffffffffffffffffffffffffffff", "ffffffff-ffff-ffff-ffff-ffffffffffff"},
        {"deadbeefdeadbeefdeadbeefdeadbeef", "deadbeef-dead-beef-dead-beefdeadbeef"},
        {"0123456789abcdef0123456789abcdef", "01234567-89ab-cdef-0123-456789abcdef"},
        {"89abcdef0123456789abcdef01234567", "89abcdef-0123-4567-89ab-cdef01234567"},
        {"a1b2c3d4e5f60718293a4b5c6d7e8f90", "a1b2c3d4-e5f6-0718-293a-4b5c6d7e8f90"},
        {"ffffffff00000000ffffffff00000000", "ffffffff-0000-0000-ffff-ffff00000000"},
        {"13579bdf2468ace013579bdf2468ace0", "13579bdf-2468-ace0-1357-9bdf2468ace0"},
        {"abcdefabcdefabcdefabcdefabcdefab", "abcdefab-cdef-abcd-efab-cdefabcdefab"},
        {"11223344556677889900aabbccddeeff", "11223344-5566-7788-9900-aabbccddeeff"},
        {"fe12dc34ba56a9876543210fedcba987", "fe12dc34-ba56-a987-6543-210fedcba987"},
    };

    for (const auto& test_case : cases) {
        EXPECT_EQ(util::SanitizeUuid(test_case.input), test_case.expected);
    }
}

TEST(StringUtilTest, InvalidLength) {
    EXPECT_THROW(util::SanitizeUuid(""), std::invalid_argument);
    EXPECT_THROW(util::SanitizeUuid("123e4567e89b12d3a45642661417400"), std::invalid_argument);
    EXPECT_THROW(util::SanitizeUuid("123e4567e89b12d3a4564266141740000"), std::invalid_argument);
}

TEST(StringUtilTest, InvalidCharacters) {
    EXPECT_THROW(util::SanitizeUuid("123e4567e89b12d3a45642661417400g"), std::invalid_argument);
    EXPECT_THROW(util::SanitizeUuid("123e4567e89b12d3a45642661417400-"), std::invalid_argument);
    EXPECT_THROW(util::SanitizeUuid("123e4567e89b12d3a4564266141740 0"), std::invalid_argument);
}

TEST(StringUtilTest, RejectUuidWithDashes) {
    std::string input = "123e4567-e89b-12d3-a456-426614174000";
    EXPECT_EQ(util::SanitizeUuid(input), input);
}

TEST(StringUtilTest, AcceptDashedUuidUppercase) {
    std::string input = "123E4567-E89B-12D3-A456-426614174000";
    EXPECT_EQ(util::SanitizeUuid(input), input);
}

TEST(StringUtilTest, RejectDashedUuidWithInvalidDashPositions) {
    EXPECT_THROW(util::SanitizeUuid("123e4567e-89b-12d3-a456-426614174000"), std::invalid_argument);
    EXPECT_THROW(util::SanitizeUuid("123e4567-e89b12d3-a456-426614174000"), std::invalid_argument);
}

TEST(StringUtilTest, RejectDashedUuidWithNonHexCharacters) {
    EXPECT_THROW(util::SanitizeUuid("123e4567-e89b-12d3-a456-42661417400g"), std::invalid_argument);
}

TEST(StringUtilTest, CopyStringToBufferExactFit) {
    const std::string input = "hello";
    char buffer[6] = {};
    EXPECT_EQ(util::CopyStringToBuffer(buffer, input, sizeof(buffer)), 5u);
    EXPECT_STREQ(buffer, "hello");
}

TEST(StringUtilTest, CopyStringToBufferTruncates) {
    const std::string input = "abcdef";
    char buffer[5] = {};
    EXPECT_EQ(util::CopyStringToBuffer(buffer, input, sizeof(buffer)), 4u);
    EXPECT_STREQ(buffer, "abcd");
}

TEST(StringUtilTest, CopyStringToBufferHandlesEmpty) {
    const std::string input;
    char buffer[3] = {'x', 'y', 'z'};
    EXPECT_EQ(util::CopyStringToBuffer(buffer, input, sizeof(buffer)), 0u);
    EXPECT_STREQ(buffer, "");
}

TEST(StringUtilTest, CopyStringToBufferChineseUtf8) {
    const std::string input = "你好世界";
    char buffer[10] = {};
    EXPECT_EQ(util::CopyStringToBuffer(buffer, input, sizeof(buffer)), 9u);
    EXPECT_STREQ(buffer, "你好世");
}

TEST(StringUtilTest, CopyStringToBufferZeroSize) {
    const std::string input = "data";
    char buffer[4] = {'a', 'b', 'c', 'd'};
    EXPECT_EQ(util::CopyStringToBuffer(buffer, input, 0), 0u);
    EXPECT_EQ(buffer[0], 'a');
}
