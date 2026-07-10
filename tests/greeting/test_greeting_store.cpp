#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "greeting/greeting_store.hpp"
#include "greeting_store_test_driver.hpp"
#include "halcyon/halcyon.hpp"
#include "spdlog/sinks/ostream_sink.h"
#include "spdlog/spdlog.h"

namespace greeting::testing {

class GreetingStoreTestPeer {
 public:
  static GreetingStore FromDatabase(halcyon::Database db) {
    return GreetingStore(
        std::make_unique<halcyon::Database>(std::move(db)));
  }
};

}  // namespace greeting::testing

namespace {
using Outcome = greeting::testing::GreetingStoreTestDriver::Outcome;

greeting::GreetingStore MakeStore(Outcome outcome) {
  auto driver =
      std::make_shared<greeting::testing::GreetingStoreTestDriver>(outcome);
  halcyon::PoolConfig config;
  config.min = 1;
  config.max = 1;
  config.startMaintenanceThread = false;
  config.statementCacheSize = 0;
  auto db = halcyon::Database::open(driver, "test-connection", config);
  if (!db.ok()) throw std::runtime_error(db.error().message);
  return greeting::testing::GreetingStoreTestPeer::FromDatabase(
      std::move(db.value()));
}

class ScopedLogCapture {
 public:
  ScopedLogCapture()
      : previous_(spdlog::default_logger()),
        logger_(std::make_shared<spdlog::logger>(
            "greeting-store-test",
            std::make_shared<spdlog::sinks::ostream_sink_mt>(stream_))) {
    spdlog::set_default_logger(logger_);
  }

  ~ScopedLogCapture() { spdlog::set_default_logger(previous_); }

  std::string str() const { return stream_.str(); }

 private:
  std::ostringstream stream_;
  std::shared_ptr<spdlog::logger> previous_;
  std::shared_ptr<spdlog::logger> logger_;
};

bool has_dsn() {
  const char* v = std::getenv("DB2_CONN_STR");
  return v && *v != '\0';
}
}  // namespace

TEST(GreetingStore, GreetingForReturnsMappedSalutationWithoutLiveDb2) {
  auto store = MakeStore(Outcome::kGreeting);
  EXPECT_EQ(store.GreetingFor("alice"), "Bonjour");
}

TEST(GreetingStore, GreetingForFallsBackWithoutThrowingOnMappingError) {
  ScopedLogCapture logs;
  auto store = MakeStore(Outcome::kNullSalutation);
  EXPECT_NO_THROW(EXPECT_EQ(store.GreetingFor("alice"), "Hello"));
  EXPECT_NE(logs.str().find("greeting row mapping failed"), std::string::npos);
}

TEST(GreetingStore, GreetingForLogsAndFallsBackOnFetchError) {
  ScopedLogCapture logs;
  auto store = MakeStore(Outcome::kFetchError);
  EXPECT_NO_THROW(EXPECT_EQ(store.GreetingFor("alice"), "Hello"));
  EXPECT_NE(logs.str().find("greeting result fetch failed"), std::string::npos);
}

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
