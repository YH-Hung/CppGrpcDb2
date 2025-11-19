// Modern C++ RAII wrapper for IBM DB2 CLI (ODBC-like) driver
// Public API header. Implementation resides under src/db2.
//
// Features:
// - Safe resource management (RAII) for ENV/DBC/STMT
// - Connect via DSN/UID/PWD or full connection string
// - Execute SQL (with or without parameters)
// - Query with row-mapping callback to user-defined struct
// - Thread-safe: operations on a single Connection are serialized
// - Exceptions with detailed diagnostic messages on errors

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <variant>
#include <optional>
#include <mutex>
#include <functional>
#include <cstdint>
#include <utility>

namespace db2 {

// Parameter value for prepared statements
using ParamValue = std::variant<std::nullptr_t, int32_t, int64_t, double, std::string>;

struct Param {
  ParamValue value{};

  // Helper constructors for convenience
  Param(std::nullptr_t) : value(nullptr) {}
  Param(int32_t v) : value(v) {}
  Param(int64_t v) : value(v) {}
  Param(double v) : value(v) {}
  Param(std::string v) : value(std::move(v)) {}
};

class Connection {
public:
  // A lightweight row view for mapping query results
  class Row {
  public:
    // Access by 1-based column index (per ODBC/CLI convention)
    std::optional<int32_t> getInt32(int col) const;
    std::optional<int64_t> getInt64(int col) const;
    std::optional<double>  getDouble(int col) const;
    std::optional<std::string> getString(int col) const;

    // Row is a non-owning view tied to the lifetime of an active statement.
    // To prevent escaping the callback and becoming dangling, disallow copy/move.
    Row(const Row&) = delete;
    Row& operator=(const Row&) = delete;
    Row(Row&&) = delete;
    Row& operator=(Row&&) = delete;

  private:
    friend class Connection;
    explicit Row(std::uintptr_t hstmt) : hstmt_(hstmt) {}
    std::uintptr_t hstmt_{}; // HSTMT stored as integral to avoid exposing CLI headers here
  };

  Connection();
  ~Connection() noexcept;

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&&) noexcept;
  Connection& operator=(Connection&&) noexcept;

  // Establish a connection using DSN + UID + PWD
  void connect_with_dsn(std::string_view dsn, std::string_view uid, std::string_view pwd);

  // Establish a connection using a full connection string
  // Example: "DATABASE=db;HOSTNAME=host;PORT=50000;PROTOCOL=TCPIP;UID=user;PWD=pass;"
  void connect_with_conn_str(std::string_view conn_str);

  bool is_connected() const noexcept;
  void disconnect() noexcept;

  // Execute a non-query SQL statement (DDL/DML without result set)
  void execute(std::string_view sql);
  void execute(std::string_view sql, const std::vector<Param>& params);

  // Execute a query and map each row to a user-defined type using the mapper.
  // Mapper signature: T mapper(const Row&)
  template <class T, class Mapper>
  std::vector<T> query(std::string_view sql, Mapper&& mapper) {
    return query_impl<T>(sql, nullptr, 0, std::forward<Mapper>(mapper));
  }

  template <class T, class Mapper>
  std::vector<T> query(std::string_view sql, const std::vector<Param>& params, Mapper&& mapper) {
    return query_impl<T>(sql, params.data(), static_cast<int>(params.size()), std::forward<Mapper>(mapper));
  }

private:
  // PIMPL-friendly internal pointers (avoid exposing DB2 headers in this public header)
  std::uintptr_t henv_{0};
  std::uintptr_t hdbc_{0};
  bool connected_{false};
  mutable std::mutex mtx_{}; // Serialize operations on the connection

  // Reconnection support
  enum class ConnMode { None, Dsn, ConnStr };
  ConnMode mode_{ConnMode::None};
  std::string dsn_{};
  std::string uid_{};
  std::string pwd_{};
  std::string conn_str_{};

  void ensure_connected_locked();
  void cleanup_locked() noexcept;
  bool try_reconnect_locked() noexcept; // attempt reconnect using stored parameters

  void execute_prepared_locked(std::string_view sql, const Param* params, int param_count);

  template <class T, class Mapper>
  std::vector<T> query_impl(std::string_view sql, const Param* params, int param_count, Mapper&& mapper);

  // Non-templated core that executes a query and invokes a callback per row
  void query_to_callback(std::string_view sql, const Param* params, int param_count,
                         const std::function<void(const Row&)>& on_row);
};

// ---- Template implementations -------------------------------------------------

template <class T, class Mapper>
inline std::vector<T> Connection::query_impl(std::string_view sql, const Param* params, int param_count, Mapper&& mapper) {
  std::vector<T> out;
  this->query_to_callback(sql, params, param_count, [&](const Connection::Row& row){
    out.emplace_back(mapper(row));
  });
  return out;
}

} // namespace db2
