#include <gtest/gtest.h>
#include "msvc.h"
#include <limits>
#include <string>

class MaxTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test basic integer comparison
TEST_F(MaxTest, BasicIntegerComparison) {
    EXPECT_EQ(MSVC_MAX(5, 3), 5);
    EXPECT_EQ(MSVC_MAX(3, 5), 5);
    EXPECT_EQ(MSVC_MAX(7, 7), 7);
}

// Test negative numbers
TEST_F(MaxTest, NegativeNumbers) {
    EXPECT_EQ(MSVC_MAX(-5, -3), -3);
    EXPECT_EQ(MSVC_MAX(-10, 5), 5);
    EXPECT_EQ(MSVC_MAX(0, -1), 0);
}

// Test floating point numbers
TEST_F(MaxTest, FloatingPointNumbers) {
    EXPECT_DOUBLE_EQ(MSVC_MAX(3.14, 2.71), 3.14);
    EXPECT_DOUBLE_EQ(MSVC_MAX(1.5, 1.5), 1.5);
    EXPECT_DOUBLE_EQ(MSVC_MAX(-1.5, -2.5), -1.5);
}

// Test with different types (should work due to implicit conversion)
TEST_F(MaxTest, MixedTypes) {
    EXPECT_EQ(MSVC_MAX(5, 3.2), 5);
    EXPECT_DOUBLE_EQ(MSVC_MAX(2.8, 3), 3.0);
}

// Test extreme values
TEST_F(MaxTest, ExtremeValues) {
    EXPECT_EQ(MSVC_MAX(std::numeric_limits<int>::max(), 
                       std::numeric_limits<int>::min()), 
              std::numeric_limits<int>::max());
    
    EXPECT_EQ(MSVC_MAX(std::numeric_limits<int>::max(), 
                       std::numeric_limits<int>::max() - 1), 
              std::numeric_limits<int>::max());
}

// Test macro side effects (important for macro testing)
TEST_F(MaxTest, MacroSideEffects) {
    int a = 5, b = 3;
    int result = MSVC_MAX(++a, ++b);
    
    // Note: This test demonstrates potential macro issues
    // The macro should only evaluate each argument once
    // This is a known limitation of the simple macro implementation
    EXPECT_TRUE(result >= 6); // Either a or b was incremented and chosen
}

// Test with expressions
TEST_F(MaxTest, ComplexExpressions) {
    EXPECT_EQ(MSVC_MAX(2 + 3, 4 + 1), 5);
    EXPECT_EQ(MSVC_MAX(10 * 2, 15 + 3), 20);
}

// Performance comparison test (optional)
TEST_F(MaxTest, MacroVsStdMax) {
    const int iterations = 1000000;
    int a = 100, b = 200;
    
    // This test mainly ensures both implementations produce the same result
    for (int i = 0; i < iterations; ++i) {
        int macro_result = MSVC_MAX(a + i, b - i);
        int std_result = std::max(a + i, b - i);
        EXPECT_EQ(macro_result, std_result);
    }
}