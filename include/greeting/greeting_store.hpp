// Db2-backed greeting lookup, built on the Halcyon client.
// This is the only unit in CppGrpcDb2 that talks to Halcyon directly.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace halcyon { class Database; }

namespace greeting {

// Wraps a pooled halcyon::Database and exposes a tiny, request-friendly API.
// Construct only via OpenFromEnv(), so a returned store is always usable.
class GreetingStore {
 public:
  GreetingStore(GreetingStore&&) noexcept;
  GreetingStore& operator=(GreetingStore&&) noexcept;
  GreetingStore(const GreetingStore&) = delete;
  GreetingStore& operator=(const GreetingStore&) = delete;
  ~GreetingStore();

  // Opens a pooled Database from the DB2_CONN_STR environment variable.
  // Returns std::nullopt (and logs) when the var is unset or open fails.
  static std::optional<GreetingStore> OpenFromEnv();

  // Idempotently (re)creates and seeds the `greetings` demo table. Safe to call
  // on every startup; DROP errors (table absent) are ignored.
  void EnsureSchema();

  // Returns the stored salutation for `name` (case-insensitive), or "Hello"
  // when no row matches or the query fails. Never throws.
  std::string GreetingFor(std::string_view name);

 private:
  explicit GreetingStore(std::unique_ptr<halcyon::Database> db);
  std::unique_ptr<halcyon::Database> db_;
};

}  // namespace greeting
