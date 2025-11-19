#include "db2/db2.hpp"

#include <stdexcept>
#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <type_traits>
#include <limits>

// Include DB2 CLI umbrella header (brings in required ODBC types)
#include <sqlcli1.h>

namespace {

inline SQLCHAR* to_sqlchar(const char* s) {
  return const_cast<SQLCHAR*>(reinterpret_cast<const SQLCHAR*>(s));
}

inline SQLSMALLINT safe_smallint(size_t n, const char* what) {
  if (n > static_cast<size_t>(std::numeric_limits<SQLSMALLINT>::max())) {
    throw std::length_error(std::string("length exceeds SQLSMALLINT for ") + what);
  }
  return static_cast<SQLSMALLINT>(n);
}

inline SQLINTEGER safe_integer(size_t n, const char* what) {
  if (n > static_cast<size_t>(std::numeric_limits<SQLINTEGER>::max())) {
    throw std::length_error(std::string("length exceeds SQLINTEGER for ") + what);
  }
  return static_cast<SQLINTEGER>(n);
}

inline std::string diag_message(SQLSMALLINT handleType, SQLHANDLE handle) {
  SQLSMALLINT i = 1;
  SQLINTEGER nativeError = 0;
  SQLCHAR sqlState[6] = {0};
  SQLCHAR messageText[1024];
  SQLSMALLINT textLength = 0;
  std::ostringstream oss;

  while (true) {
    SQLRETURN rc = SQLGetDiagRec(handleType, handle, i, sqlState, &nativeError,
                                 messageText, sizeof(messageText), &textLength);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) break;
    oss << "[" << sqlState << "] (" << nativeError << ") "
        << std::string(reinterpret_cast<char*>(messageText), textLength);
    ++i;
    if (i > 20) break; // avoid infinite loops
    if (i > 1) oss << " | ";
  }
  std::string out = oss.str();
  if (out.empty()) out = "DB2 CLI error (no diagnostics)";
  return out;
}

inline void throw_diag(SQLSMALLINT handleType, SQLHANDLE handle, const char* where) {
  std::ostringstream oss;
  oss << where << ": " << diag_message(handleType, handle);
  throw std::runtime_error(oss.str());
}

inline std::string first_sql_state(SQLSMALLINT handleType, SQLHANDLE handle) {
  SQLCHAR sqlState[6] = {0};
  SQLRETURN rc = SQLGetDiagRec(handleType, handle, 1, sqlState, nullptr, nullptr, 0, nullptr);
  if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
    return std::string(reinterpret_cast<char*>(sqlState));
  }
  return {};
}

inline bool is_connection_broken_sqlstate(std::string_view state) {
  if (state.size() >= 2 && state[0] == '0' && state[1] == '8') return true; // 08xxx
  // Common CLI/ODBC timeouts/comm failures that imply reconnect
  return (state == "40003" || state == "HYT00" || state == "HYT01" || state == "58004");
}

// Non-null placeholder for NULL parameter bindings (some drivers dereference ValuePtr even for NULL)
static unsigned char g_null_param_dummy = 0;

} // namespace

namespace db2 {

using HENV  = SQLHENV;
using HDBC  = SQLHDBC;
using HSTMT = SQLHSTMT;

template <typename H>
static inline std::uintptr_t store_handle(H h) {
  if constexpr (std::is_pointer_v<H>) {
    return reinterpret_cast<std::uintptr_t>(h);
  } else {
    return static_cast<std::uintptr_t>(h);
  }
}

template <typename H>
static inline H load_handle(std::uintptr_t v) {
  if constexpr (std::is_pointer_v<H>) {
    return reinterpret_cast<H>(v);
  } else {
    return static_cast<H>(v);
  }
}

Connection::Connection() {
  // Allocate environment
  HENV henv{};
  SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw std::runtime_error("Failed to allocate DB2 environment handle");
  }

  // Set ODBC version 3
  rc = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    std::string msg = diag_message(SQL_HANDLE_ENV, henv);
    SQLFreeHandle(SQL_HANDLE_ENV, henv);
    throw std::runtime_error("Failed to set ODBC version: " + msg);
  }

  // Allocate connection
  HDBC hdbc{};
  rc = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    std::string msg = diag_message(SQL_HANDLE_ENV, henv);
    SQLFreeHandle(SQL_HANDLE_ENV, henv);
    throw std::runtime_error("Failed to allocate DB2 connection handle: " + msg);
  }

  henv_ = store_handle(henv);
  hdbc_ = store_handle(hdbc);
}

Connection::~Connection() noexcept {
  std::scoped_lock lk(mtx_);
  cleanup_locked();
}

Connection::Connection(Connection&& other) noexcept {
  std::scoped_lock lk(other.mtx_);
  henv_ = other.henv_;
  hdbc_ = other.hdbc_;
  connected_ = other.connected_;
  mode_ = other.mode_;
  dsn_ = std::move(other.dsn_);
  uid_ = std::move(other.uid_);
  pwd_ = std::move(other.pwd_);
  conn_str_ = std::move(other.conn_str_);
  other.henv_ = 0;
  other.hdbc_ = 0;
  other.connected_ = false;
  other.mode_ = ConnMode::None;
}

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this == &other) return *this;
  std::scoped_lock lk(mtx_, other.mtx_);
  cleanup_locked();
  henv_ = other.henv_;
  hdbc_ = other.hdbc_;
  connected_ = other.connected_;
  mode_ = other.mode_;
  dsn_ = std::move(other.dsn_);
  uid_ = std::move(other.uid_);
  pwd_ = std::move(other.pwd_);
  conn_str_ = std::move(other.conn_str_);
  other.henv_ = 0;
  other.hdbc_ = 0;
  other.connected_ = false;
  other.mode_ = ConnMode::None;
  return *this;
}

bool Connection::is_connected() const noexcept {
  std::scoped_lock lk(mtx_);
  return connected_;
}

void Connection::ensure_connected_locked() {
  if (!connected_) {
    throw std::runtime_error("DB2 connection is not established");
  }
}

void Connection::connect_with_dsn(std::string_view dsn, std::string_view uid, std::string_view pwd) {
  std::scoped_lock lk(mtx_);
  if (connected_) return; // already connected
  auto hdbc = load_handle<HDBC>(hdbc_);
  // Use SQL_NTS to avoid length narrowing, ensure null-terminated buffers
  std::string dsn_s(dsn);
  std::string uid_s(uid);
  std::string pwd_s(pwd);
  SQLRETURN rc = SQLConnect(
      hdbc,
      to_sqlchar(dsn_s.c_str()), SQL_NTS,
      to_sqlchar(uid_s.c_str()), SQL_NTS,
      to_sqlchar(pwd_s.c_str()), SQL_NTS);
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw_diag(SQL_HANDLE_DBC, hdbc, "SQLConnect");
  }
  connected_ = true;
  // Store for potential auto-reconnect
  mode_ = ConnMode::Dsn;
  dsn_.assign(dsn.begin(), dsn.end());
  uid_.assign(uid.begin(), uid.end());
  pwd_.assign(pwd.begin(), pwd.end());
  conn_str_.clear();
}

void Connection::connect_with_conn_str(std::string_view conn_str) {
  std::scoped_lock lk(mtx_);
  if (connected_) return;
  auto hdbc = load_handle<HDBC>(hdbc_);
  SQLCHAR outConn[1024];
  SQLSMALLINT outLen = 0;
  std::string cs(conn_str);
  SQLRETURN rc = SQLDriverConnect(
      hdbc, nullptr,
      to_sqlchar(cs.c_str()), SQL_NTS,
      outConn, safe_smallint(sizeof(outConn), "SQLDriverConnect out buffer"), &outLen,
      SQL_DRIVER_NOPROMPT);
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw_diag(SQL_HANDLE_DBC, hdbc, "SQLDriverConnect");
  }
  connected_ = true;
  // Store for potential auto-reconnect
  mode_ = ConnMode::ConnStr;
  conn_str_.assign(conn_str.begin(), conn_str.end());
  dsn_.clear(); uid_.clear(); pwd_.clear();
}

void Connection::disconnect() noexcept {
  std::scoped_lock lk(mtx_);
  if (connected_ && hdbc_ != 0) {
    SQLDisconnect(load_handle<HDBC>(hdbc_));
    connected_ = false;
  }
}

void Connection::cleanup_locked() noexcept {
  if (hdbc_ != 0) {
    if (connected_) {
      SQLDisconnect(load_handle<HDBC>(hdbc_));
      connected_ = false;
    }
    SQLFreeHandle(SQL_HANDLE_DBC, load_handle<HDBC>(hdbc_));
    hdbc_ = 0;
  }
  if (henv_ != 0) {
    SQLFreeHandle(SQL_HANDLE_ENV, load_handle<HENV>(henv_));
    henv_ = 0;
  }
}

bool Connection::try_reconnect_locked() noexcept {
  // Assumes mtx_ is held
  if (hdbc_ == 0 || henv_ == 0) return false;
  auto hdbc = load_handle<HDBC>(hdbc_);
  // Attempt to disconnect first, ignore errors
  SQLDisconnect(hdbc);
  connected_ = false;

  SQLRETURN rc = SQL_ERROR;
  switch (mode_) {
    case ConnMode::Dsn: {
      if (dsn_.empty()) return false;
      rc = SQLConnect(
          hdbc,
          to_sqlchar(dsn_.c_str()), SQL_NTS,
          to_sqlchar(uid_.c_str()), SQL_NTS,
          to_sqlchar(pwd_.c_str()), SQL_NTS);
      break;
    }
    case ConnMode::ConnStr: {
      if (conn_str_.empty()) return false;
      SQLCHAR outConn[1024];
      SQLSMALLINT outLen = 0;
      rc = SQLDriverConnect(
          hdbc, nullptr,
          to_sqlchar(conn_str_.c_str()), SQL_NTS,
          outConn, safe_smallint(sizeof(outConn), "SQLDriverConnect out buffer"), &outLen,
          SQL_DRIVER_NOPROMPT);
      break;
    }
    case ConnMode::None:
    default:
      return false;
  }

  if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
    connected_ = true;
    return true;
  }
  return false;
}

void Connection::execute(std::string_view sql) {
  std::scoped_lock lk(mtx_);
  ensure_connected_locked();
  auto hdbc = load_handle<HDBC>(hdbc_);
  int attempts = 0;
  for (;;) {
    // RAII guard for statement handle
    struct StmtGuard {
      HSTMT h{};
      explicit StmtGuard(HDBC dbc) {
        SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, dbc, &h);
        if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
          h = 0;
        }
      }
      ~StmtGuard() {
        if (h) { SQLCloseCursor(h); SQLFreeHandle(SQL_HANDLE_STMT, h); }
      }
    } stmt(hdbc);

    SQLRETURN rc = (stmt.h ? SQL_SUCCESS : SQL_ERROR);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
      std::string st = first_sql_state(SQL_HANDLE_DBC, hdbc);
      if (attempts == 0 && is_connection_broken_sqlstate(st) && try_reconnect_locked()) {
        ++attempts; hdbc = load_handle<HDBC>(hdbc_); continue; // retry
      }
      throw_diag(SQL_HANDLE_DBC, hdbc, "SQLAllocHandle(SQL_HANDLE_STMT)");
    }

    std::string sql_s(sql);
    rc = SQLExecDirect(stmt.h, to_sqlchar(sql_s.c_str()), SQL_NTS);
    if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
      return;
    }

    std::string st = first_sql_state(SQL_HANDLE_STMT, stmt.h);
    std::string msg = diag_message(SQL_HANDLE_STMT, stmt.h);
    if (attempts == 0 && is_connection_broken_sqlstate(st) && try_reconnect_locked()) {
      ++attempts; hdbc = load_handle<HDBC>(hdbc_); continue; // retry once
    }
    throw std::runtime_error("SQLExecDirect failed: " + msg);
  }
}

void Connection::execute(std::string_view sql, const std::vector<Param>& params) {
  std::scoped_lock lk(mtx_);
  ensure_connected_locked();
  int attempts = 0;
  for (;;) {
    try {
      execute_prepared_locked(sql, params.data(), static_cast<int>(params.size()));
      return;
    } catch (const std::runtime_error& ex) {
      // Heuristically decide if we should reconnect based on last diagnostics on DBC
      auto hdbc = load_handle<HDBC>(hdbc_);
      std::string st = first_sql_state(SQL_HANDLE_DBC, hdbc);
      if (attempts == 0 && is_connection_broken_sqlstate(st) && try_reconnect_locked()) {
        ++attempts; continue; // retry once
      }
      throw; // propagate original
    }
  }
}

void Connection::execute_prepared_locked(std::string_view sql, const Param* params, int param_count) {
  auto hdbc = load_handle<HDBC>(hdbc_);
  int attempts = 0;
  for (;;) {
    struct StmtGuard {
      HSTMT h{};
      explicit StmtGuard(HDBC dbc) {
        SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, dbc, &h);
        if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) { h = 0; }
      }
      ~StmtGuard() { if (h) { SQLCloseCursor(h); SQLFreeHandle(SQL_HANDLE_STMT, h);} }
    } stmt(hdbc);
    SQLRETURN rc = (stmt.h ? SQL_SUCCESS : SQL_ERROR);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
      std::string st = first_sql_state(SQL_HANDLE_DBC, hdbc);
      if (attempts == 0 && is_connection_broken_sqlstate(st) && try_reconnect_locked()) {
        ++attempts; hdbc = load_handle<HDBC>(hdbc_); continue;
      }
      throw_diag(SQL_HANDLE_DBC, hdbc, "SQLAllocHandle(SQL_HANDLE_STMT)");
    }

    std::string sql_s(sql);
    rc = SQLPrepare(stmt.h, to_sqlchar(sql_s.c_str()), SQL_NTS);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
      std::string st = first_sql_state(SQL_HANDLE_STMT, stmt.h);
      std::string msg = diag_message(SQL_HANDLE_STMT, stmt.h);
      if (attempts == 0 && is_connection_broken_sqlstate(st) && try_reconnect_locked()) {
        ++attempts; hdbc = load_handle<HDBC>(hdbc_); continue;
      }
      throw std::runtime_error("SQLPrepare failed: " + msg);
    }

    // Storage for bound values and indicators to keep memory alive
    std::vector<int32_t> i32_vals; i32_vals.reserve(param_count);
    std::vector<int64_t> i64_vals; i64_vals.reserve(param_count);
    std::vector<double>  dbl_vals; dbl_vals.reserve(param_count);
    std::vector<std::string> str_vals; str_vals.reserve(param_count);
    std::vector<SQLLEN> ind_vals(param_count, 0);

    bool bind_ok = true;
    for (int i = 0; i < param_count; ++i) {
      SQLUSMALLINT paramNum = static_cast<SQLUSMALLINT>(i + 1);
      SQLSMALLINT cType = 0;
      SQLSMALLINT sqlType = 0;
      SQLULEN colDef = 0;
      SQLSMALLINT scale = 0;
      SQLPOINTER valPtr = nullptr;
      SQLLEN* indPtr = &ind_vals[i];

      const auto& v = params[i].value;
      SQLLEN buffer_len = 0;
      if (std::holds_alternative<std::nullptr_t>(v)) {
        // Use non-null dummy pointer and mark indicator as NULL
        cType = SQL_C_CHAR; sqlType = SQL_VARCHAR; colDef = 1; scale = 0; valPtr = &g_null_param_dummy; *indPtr = SQL_NULL_DATA; buffer_len = 1;
      } else if (auto pv = std::get_if<int32_t>(&v)) {
        cType = SQL_C_SLONG; sqlType = SQL_INTEGER; i32_vals.push_back(*pv); valPtr = &i32_vals.back(); *indPtr = sizeof(int32_t); buffer_len = sizeof(int32_t);
      } else if (auto pv = std::get_if<int64_t>(&v)) {
        cType = SQL_C_SBIGINT; sqlType = SQL_BIGINT; i64_vals.push_back(*pv); valPtr = &i64_vals.back(); *indPtr = sizeof(int64_t); buffer_len = sizeof(int64_t);
      } else if (auto pv = std::get_if<double>(&v)) {
        cType = SQL_C_DOUBLE; sqlType = SQL_DOUBLE; dbl_vals.push_back(*pv); valPtr = &dbl_vals.back(); *indPtr = sizeof(double); buffer_len = sizeof(double);
      } else if (auto pv = std::get_if<std::string>(&v)) {
        cType = SQL_C_CHAR; sqlType = SQL_VARCHAR;
        // Use a safe column definition to avoid truncation metadata issues
        const SQLULEN actual = static_cast<SQLULEN>(pv->size());
        const SQLULEN safe_def = std::max<SQLULEN>(actual, 4096);
        colDef = std::max<SQLULEN>(safe_def, 1);
        scale = 0;
        str_vals.push_back(*pv);
        valPtr = reinterpret_cast<SQLPOINTER>(str_vals.back().data());
        *indPtr = SQL_NTS; // null-terminated
        buffer_len = static_cast<SQLLEN>(str_vals.back().size() + 1);
      }

      rc = SQLBindParameter(stmt.h, paramNum, SQL_PARAM_INPUT,
                            cType, sqlType, colDef, scale,
                            valPtr, buffer_len, indPtr);
      if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        std::string st = first_sql_state(SQL_HANDLE_STMT, stmt.h);
        std::string msg = diag_message(SQL_HANDLE_STMT, stmt.h);
        if (attempts == 0 && is_connection_broken_sqlstate(st) && try_reconnect_locked()) {
          ++attempts; hdbc = load_handle<HDBC>(hdbc_); bind_ok = false; break; // retry
        }
        throw std::runtime_error("SQLBindParameter failed: " + msg);
      }
    }
    if (!bind_ok) continue; // go retry

    rc = SQLExecute(stmt.h);
    if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
      return;
    }

    std::string st = first_sql_state(SQL_HANDLE_STMT, stmt.h);
    std::string msg = diag_message(SQL_HANDLE_STMT, stmt.h);
    if (attempts == 0 && is_connection_broken_sqlstate(st) && try_reconnect_locked()) {
      ++attempts; hdbc = load_handle<HDBC>(hdbc_); continue; // retry once
    }
    throw std::runtime_error("SQLExecute failed: " + msg);
  }
}

void Connection::query_to_callback(std::string_view sql, const Param* params, int param_count,
                                   const std::function<void(const Row&)>& on_row) {
  std::scoped_lock lk(mtx_);
  ensure_connected_locked();

  auto hdbc = load_handle<HDBC>(hdbc_);
  int attempts = 0;
  bool prepared = (params != nullptr && param_count > 0);
  bool delivered_any = false;

  auto run_once = [&](HDBC use_hdbc, int& out_rows) -> std::pair<bool, std::string> {
    out_rows = 0;
    struct StmtGuard { HSTMT h{}; explicit StmtGuard(HDBC dbc){ SQLRETURN r=SQLAllocHandle(SQL_HANDLE_STMT, dbc, &h); if(!(r==SQL_SUCCESS||r==SQL_SUCCESS_WITH_INFO)) h=0;} ~StmtGuard(){ if(h){ SQLCloseCursor(h); SQLFreeHandle(SQL_HANDLE_STMT, h);} } } stmt(use_hdbc);
    SQLRETURN rc = (stmt.h ? SQL_SUCCESS : SQL_ERROR);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
      std::string st = first_sql_state(SQL_HANDLE_DBC, use_hdbc);
      if (st.empty()) st = "HY000";
      return {false, st};
    }

    if (prepared) {
      std::string sql_s(sql);
      rc = SQLPrepare(stmt.h, to_sqlchar(sql_s.c_str()), SQL_NTS);
      if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        std::string st = first_sql_state(SQL_HANDLE_STMT, stmt.h);
        if (st.empty()) st = first_sql_state(SQL_HANDLE_DBC, use_hdbc);
        if (st.empty()) st = "HY000";
        return {false, st};
      }

      std::vector<int32_t> i32_vals; i32_vals.reserve(param_count);
      std::vector<int64_t> i64_vals; i64_vals.reserve(param_count);
      std::vector<double>  dbl_vals; dbl_vals.reserve(param_count);
      std::vector<std::string> str_vals; str_vals.reserve(param_count);
      std::vector<SQLLEN> ind_vals(param_count, 0);

      for (int i = 0; i < param_count; ++i) {
        SQLUSMALLINT paramNum = static_cast<SQLUSMALLINT>(i + 1);
        SQLSMALLINT cType = 0; SQLSMALLINT sqlType = 0; SQLULEN colDef = 0; SQLSMALLINT scale = 0; SQLPOINTER valPtr = nullptr; SQLLEN* indPtr = &ind_vals[i];
        const auto& v = params[i].value;
        SQLLEN buffer_len = 0;
        if (std::holds_alternative<std::nullptr_t>(v)) { cType = SQL_C_CHAR; sqlType = SQL_VARCHAR; colDef = 1; scale = 0; valPtr = &g_null_param_dummy; *indPtr = SQL_NULL_DATA; buffer_len = 1; }
        else if (auto pv = std::get_if<int32_t>(&v)) { cType = SQL_C_SLONG; sqlType = SQL_INTEGER; i32_vals.push_back(*pv); valPtr = &i32_vals.back(); *indPtr = sizeof(int32_t); buffer_len = sizeof(int32_t); }
        else if (auto pv = std::get_if<int64_t>(&v)) { cType = SQL_C_SBIGINT; sqlType = SQL_BIGINT; i64_vals.push_back(*pv); valPtr = &i64_vals.back(); *indPtr = sizeof(int64_t); buffer_len = sizeof(int64_t); }
        else if (auto pv = std::get_if<double>(&v)) { cType = SQL_C_DOUBLE; sqlType = SQL_DOUBLE; dbl_vals.push_back(*pv); valPtr = &dbl_vals.back(); *indPtr = sizeof(double); buffer_len = sizeof(double); }
        else if (auto pv = std::get_if<std::string>(&v)) { cType = SQL_C_CHAR; sqlType = SQL_VARCHAR; const SQLULEN actual = static_cast<SQLULEN>(pv->size()); const SQLULEN safe_def = std::max<SQLULEN>(actual, 4096); colDef = std::max<SQLULEN>(safe_def, 1); scale = 0; str_vals.push_back(*pv); valPtr = reinterpret_cast<SQLPOINTER>(str_vals.back().data()); *indPtr = SQL_NTS; buffer_len = static_cast<SQLLEN>(str_vals.back().size() + 1); }
        rc = SQLBindParameter(stmt.h, paramNum, SQL_PARAM_INPUT, cType, sqlType, colDef, scale, valPtr, buffer_len, indPtr);
        if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) { std::string st = first_sql_state(SQL_HANDLE_STMT, stmt.h); if (st.empty()) st = first_sql_state(SQL_HANDLE_DBC, use_hdbc); if (st.empty()) st = "HY000"; return {false, st}; }
      }

      rc = SQLExecute(stmt.h);
      if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) { std::string st = first_sql_state(SQL_HANDLE_STMT, stmt.h); if (st.empty()) st = first_sql_state(SQL_HANDLE_DBC, use_hdbc); if (st.empty()) st = "HY000"; return {false, st}; }
    } else {
      std::string sql_s(sql);
      rc = SQLExecDirect(stmt.h, to_sqlchar(sql_s.c_str()), SQL_NTS);
      if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) { std::string st = first_sql_state(SQL_HANDLE_STMT, stmt.h); if (st.empty()) st = first_sql_state(SQL_HANDLE_DBC, use_hdbc); if (st.empty()) st = "HY000"; return {false, st}; }
    }

    while (true) {
      rc = SQLFetch(stmt.h);
      if (rc == SQL_NO_DATA) break;
      if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        std::string st = first_sql_state(SQL_HANDLE_STMT, stmt.h);
        if (st.empty()) st = first_sql_state(SQL_HANDLE_DBC, use_hdbc);
        if (st.empty()) st = "HY000"; // generic error state, never empty
        return {false, st};
      }
      Row row{store_handle(stmt.h)};
      on_row(row);
      ++out_rows;
      delivered_any = true;
    }
    return {true, std::string{}};
  };

  for (;;) {
    int out_rows = 0;
    auto [ok, state] = run_once(hdbc, out_rows);
    if (ok) return;
    if (attempts == 0 && is_connection_broken_sqlstate(state) && try_reconnect_locked()) {
      ++attempts; hdbc = load_handle<HDBC>(hdbc_); continue; // retry once from scratch
    }
    // If not retried, throw diagnostics from DBC or state we have
    if (prepared) {
      throw std::runtime_error("Query failed (prepare/execute/fetch) and reconnect did not resolve the issue.");
    } else {
      throw std::runtime_error("Query failed (exec/fetch) and reconnect did not resolve the issue.");
    }
  }
}

// ---------------- Row getters ----------------

std::optional<int32_t> Connection::Row::getInt32(int col) const {
  SQLLEN ind = 0;
  int32_t val = 0;
  SQLRETURN rc = SQLGetData(load_handle<HSTMT>(hstmt_), static_cast<SQLUSMALLINT>(col), SQL_C_SLONG, &val, sizeof(val), &ind);
  if (rc == SQL_NO_DATA) return std::nullopt;
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw_diag(SQL_HANDLE_STMT, load_handle<HSTMT>(hstmt_), "SQLGetData(int32)");
  }
  if (ind == SQL_NULL_DATA) return std::nullopt;
  return val;
}

std::optional<int64_t> Connection::Row::getInt64(int col) const {
  SQLLEN ind = 0;
  int64_t val = 0;
  SQLRETURN rc = SQLGetData(load_handle<HSTMT>(hstmt_), static_cast<SQLUSMALLINT>(col), SQL_C_SBIGINT, &val, sizeof(val), &ind);
  if (rc == SQL_NO_DATA) return std::nullopt;
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw_diag(SQL_HANDLE_STMT, load_handle<HSTMT>(hstmt_), "SQLGetData(int64)");
  }
  if (ind == SQL_NULL_DATA) return std::nullopt;
  return val;
}

std::optional<double> Connection::Row::getDouble(int col) const {
  SQLLEN ind = 0;
  double val = 0.0;
  SQLRETURN rc = SQLGetData(load_handle<HSTMT>(hstmt_), static_cast<SQLUSMALLINT>(col), SQL_C_DOUBLE, &val, sizeof(val), &ind);
  if (rc == SQL_NO_DATA) return std::nullopt;
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw_diag(SQL_HANDLE_STMT, load_handle<HSTMT>(hstmt_), "SQLGetData(double)");
  }
  if (ind == SQL_NULL_DATA) return std::nullopt;
  return val;
}

std::optional<std::string> Connection::Row::getString(int col) const {
  std::string result;
  SQLLEN ind = 0;
  // First call with small buffer to get length
  char buf[256];
  SQLLEN got = 0;
  SQLRETURN rc = SQLGetData(load_handle<HSTMT>(hstmt_), static_cast<SQLUSMALLINT>(col), SQL_C_CHAR,
                            buf, sizeof(buf), &ind);
  if (rc == SQL_NO_DATA) return std::nullopt;
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw_diag(SQL_HANDLE_STMT, load_handle<HSTMT>(hstmt_), "SQLGetData(string)");
  }
  if (ind == SQL_NULL_DATA) return std::nullopt;
  if (ind <= (SQLLEN)sizeof(buf)) {
    // ind includes terminating null; exclude it
    if (ind > 0) {
      result.assign(buf, buf + std::max<SQLLEN>(0, ind - 1));
    }
    return result;
  }
  // Need to fetch remaining parts
  result.assign(buf, buf + (sizeof(buf) - 1)); // drop terminator
  SQLLEN totalExpected = ind - 1; // exclude terminator
  while ((SQLLEN)result.size() < totalExpected) {
    char chunk[512];
    rc = SQLGetData(load_handle<HSTMT>(hstmt_), static_cast<SQLUSMALLINT>(col), SQL_C_CHAR,
                    chunk, sizeof(chunk), &got);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
      throw_diag(SQL_HANDLE_STMT, load_handle<HSTMT>(hstmt_), "SQLGetData(string-continued)");
    }
    if (got == SQL_NULL_DATA) break;
    if (got > 0) {
      // got includes terminating null in last chunk
      size_t toAppend = static_cast<size_t>(got);
      if ((SQLLEN)(result.size() + toAppend) >= totalExpected + 1) {
        // last chunk, remove terminator
        if (toAppend > 0) --toAppend;
      }
      result.append(chunk, chunk + toAppend);
    }
    if (rc == SQL_SUCCESS) break; // all data retrieved
  }
  return result;
}

} // namespace db2
