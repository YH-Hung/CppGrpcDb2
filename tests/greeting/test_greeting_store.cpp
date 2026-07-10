#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>

#include "greeting/greeting_store.hpp"

namespace {
bool has_dsn() {
  const char* v = std::getenv("DB2_CONN_STR");
  return v && *v != '\0';
}
}  // namespace

TEST(GreetingStore, OpenFromEnvIsNulloptWithoutDsn) {
  if (has_dsn()) GTEST_SKIP() << "DB2_CONN_STR is set; covered by live test";
  auto store = greeting::GreetingStore::OpenFromEnv();
  EXPECT_FALSE(store.has_value());
}

TEST(GreetingStore, SeededNameReturnsSalutationLive) {
  if (!has_dsn()) GTEST_SKIP() << "DB2_CONN_STR not set; skipping live Db2 test";
  auto store = greeting::GreetingStore::OpenFromEnv();
  ASSERT_TRUE(store.has_value());
  store->EnsureSchema();
  EXPECT_EQ(store->GreetingFor("alice"), "Bonjour");
  EXPECT_EQ(store->GreetingFor("ALICE"), "Bonjour");  // case-insensitive
  EXPECT_EQ(store->GreetingFor("nobody"), "Hello");   // default fallback
}
