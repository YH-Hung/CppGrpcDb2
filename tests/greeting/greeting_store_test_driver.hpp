#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"

namespace greeting::testing {

class GreetingStoreTestDriver final
    : public halcyon::detail::cli::ICliDriver {
 public:
  using ConnectionHandle = halcyon::detail::cli::ConnectionHandle;
  using ConnectionParams = halcyon::detail::cli::ConnectionParams;
  using StatementHandle = halcyon::detail::cli::StatementHandle;
  using Value = halcyon::detail::cli::Value;

  enum class Outcome { kGreeting, kNullSalutation, kFetchError };

  explicit GreetingStoreTestDriver(Outcome outcome,
                                   std::string salutation = "Bonjour")
      : outcome_(outcome), salutation_(std::move(salutation)) {}

  halcyon::Result<ConnectionHandle> connect(
      const ConnectionParams&) override {
    return static_cast<ConnectionHandle>(1);
  }

  halcyon::Result<void> disconnect(ConnectionHandle) override { return {}; }

  halcyon::Result<bool> isAlive(ConnectionHandle) override { return true; }

  halcyon::Result<StatementHandle> prepare(ConnectionHandle,
                                           const std::string&) override {
    return static_cast<StatementHandle>(1);
  }

  halcyon::Result<void> bindParams(
      StatementHandle, const std::vector<Value>&) override {
    return {};
  }

  halcyon::Result<std::int64_t> execute(StatementHandle) override {
    fetched_ = false;
    return std::int64_t{0};
  }

  halcyon::Result<std::size_t> columnCount(StatementHandle) override {
    return std::size_t{1};
  }

  halcyon::Result<std::string> columnName(StatementHandle,
                                          std::size_t) override {
    return std::string("salutation");
  }

  halcyon::Result<bool> fetch(StatementHandle) override {
    if (outcome_ == Outcome::kFetchError && !fetched_) {
      fetched_ = true;
      return MakeError(halcyon::ErrorCode::Connection,
                       "scripted fetch failure");
    }
    if (fetched_) return false;
    fetched_ = true;
    return true;
  }

  halcyon::Result<Value> getColumn(StatementHandle, std::size_t) override {
    if (outcome_ == Outcome::kNullSalutation) {
      return Value{halcyon::detail::cli::Null{}};
    }
    return Value{salutation_};
  }

  halcyon::Result<void> finalize(StatementHandle) override { return {}; }

  halcyon::Result<void> closeCursor(StatementHandle) override { return {}; }

  halcyon::Result<void> setAutoCommit(ConnectionHandle, bool) override {
    return {};
  }

  halcyon::Result<void> commit(ConnectionHandle) override { return {}; }

  halcyon::Result<void> rollback(ConnectionHandle) override { return {}; }

 private:
  static halcyon::Error MakeError(halcyon::ErrorCode code,
                                  std::string message) {
    halcyon::Error error;
    error.code = code;
    error.message = std::move(message);
    return error;
  }

  Outcome outcome_;
  std::string salutation_;
  bool fetched_ = false;
};

}  // namespace greeting::testing
