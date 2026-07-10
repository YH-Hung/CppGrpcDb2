#include "greeting/greeting_store.hpp"

#include <cstdlib>
#include <tuple>
#include <utility>

#include "halcyon/halcyon.hpp"
#include "spdlog/spdlog.h"

namespace greeting {

namespace {
constexpr char kDefaultSalutation[] = "Hello";
}  // namespace

GreetingStore::GreetingStore(std::unique_ptr<halcyon::Database> db)
    : db_(std::move(db)) {}

GreetingStore::GreetingStore(GreetingStore&&) noexcept = default;
GreetingStore& GreetingStore::operator=(GreetingStore&&) noexcept = default;
GreetingStore::~GreetingStore() = default;

std::optional<GreetingStore> GreetingStore::OpenFromEnv() {
  const char* dsn = std::getenv("DB2_CONN_STR");
  if (!dsn || *dsn == '\0') {
    spdlog::warn("DB2_CONN_STR not set; greetings will use the default prefix");
    return std::nullopt;
  }
  halcyon::PoolConfig cfg;
  cfg.min = 2;
  cfg.max = 8;
  auto db = halcyon::Database::open(dsn, cfg);
  if (!db.ok()) {
    spdlog::error("Failed to open Db2 via Halcyon: {}", db.error().message);
    return std::nullopt;
  }
  spdlog::info("Connected to Db2 via Halcyon");
  return GreetingStore(
      std::make_unique<halcyon::Database>(std::move(db.value())));
}

void GreetingStore::EnsureSchema() {
  db_->execute("DROP TABLE greetings");  // ignore error if absent
  auto created = db_->execute(
      "CREATE TABLE greetings("
      "name VARCHAR(64) NOT NULL PRIMARY KEY, salutation VARCHAR(64))");
  if (!created.ok()) {
    spdlog::error("Failed to create greetings table: {}",
                  created.error().message);
    return;
  }
  const std::pair<const char*, const char*> seed[] = {
      {"alice", "Bonjour"}, {"bob", "Hola"}, {"yuki", "Konnichiwa"}};
  for (const auto& [name, salutation] : seed) {
    auto r = db_->execute(
        "INSERT INTO greetings(name, salutation) VALUES (?, ?)",
        std::string(name), std::string(salutation));
    if (!r.ok())
      spdlog::error("Failed to seed greeting for {}: {}", name,
                    r.error().message);
  }
  spdlog::info("greetings table ready ({} rows seeded)",
               sizeof(seed) / sizeof(seed[0]));
}

std::string GreetingStore::GreetingFor(std::string_view name) {
  auto rs = db_->query(
      "SELECT salutation FROM greetings WHERE UPPER(name) = UPPER(?)",
      std::string(name));
  if (!rs.ok()) {
    spdlog::error("greeting lookup failed for '{}': {}", name,
                  rs.error().message);
    return kDefaultSalutation;
  }

  auto& result = rs.value();
  for (auto& row : result) {
    auto salutation = row.try_as<std::string>();
    if (!salutation.ok()) {
      spdlog::error("greeting row mapping failed for '{}': {}", name,
                    salutation.error().message);
      return kDefaultSalutation;
    }
    return std::get<0>(std::move(salutation.value()));
  }

  if (!result.ok()) {
    spdlog::error("greeting result fetch failed for '{}': {}", name,
                  result.error()->message);
  }
  return kDefaultSalutation;
}

}  // namespace greeting
