#include "db2/db2.hpp"

#include <stdexcept>
#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <type_traits>

// Include DB2 CLI umbrella header (brings in required ODBC types)
#include <sqlcli1.h>

namespace {

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

Connection::~Connection() {
  std::scoped_lock lk(mtx_);
  cleanup_locked();
}

Connection::Connection(Connection&& other) noexcept {
  std::scoped_lock lk(other.mtx_);
  henv_ = other.henv_;
  hdbc_ = other.hdbc_;
  connected_ = other.connected_;
  other.henv_ = 0;
  other.hdbc_ = 0;
  other.connected_ = false;
}

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this == &other) return *this;
  std::scoped_lock lk(mtx_, other.mtx_);
  cleanup_locked();
  henv_ = other.henv_;
  hdbc_ = other.hdbc_;
  connected_ = other.connected_;
  other.henv_ = 0;
  other.hdbc_ = 0;
  other.connected_ = false;
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
  SQLRETURN rc = SQLConnect(
      hdbc,
      (SQLCHAR*)dsn.data(), static_cast<SQLSMALLINT>(dsn.size()),
      (SQLCHAR*)uid.data(), static_cast<SQLSMALLINT>(uid.size()),
      (SQLCHAR*)pwd.data(), static_cast<SQLSMALLINT>(pwd.size()));
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw_diag(SQL_HANDLE_DBC, hdbc, "SQLConnect");
  }
  connected_ = true;
}

void Connection::connect_with_conn_str(std::string_view conn_str) {
  std::scoped_lock lk(mtx_);
  if (connected_) return;
  auto hdbc = load_handle<HDBC>(hdbc_);
  SQLCHAR outConn[1024];
  SQLSMALLINT outLen = 0;
  SQLRETURN rc = SQLDriverConnect(
      hdbc, nullptr,
      (SQLCHAR*)conn_str.data(), static_cast<SQLSMALLINT>(conn_str.size()),
      outConn, sizeof(outConn), &outLen,
      SQL_DRIVER_NOPROMPT);
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw_diag(SQL_HANDLE_DBC, hdbc, "SQLDriverConnect");
  }
  connected_ = true;
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

void Connection::execute(std::string_view sql) {
  std::scoped_lock lk(mtx_);
  ensure_connected_locked();
  HSTMT hstmt{};
  SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, load_handle<HDBC>(hdbc_), &hstmt);
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw_diag(SQL_HANDLE_DBC, load_handle<HDBC>(hdbc_), "SQLAllocHandle(SQL_HANDLE_STMT)");
  }
  rc = SQLExecDirect(hstmt, (SQLCHAR*)sql.data(), static_cast<SQLINTEGER>(sql.size()));
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    std::string msg = diag_message(SQL_HANDLE_STMT, hstmt);
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    throw std::runtime_error("SQLExecDirect failed: " + msg);
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
}

void Connection::execute(std::string_view sql, const std::vector<Param>& params) {
  std::scoped_lock lk(mtx_);
  ensure_connected_locked();
  execute_prepared_locked(sql, params.data(), static_cast<int>(params.size()));
}

void Connection::execute_prepared_locked(std::string_view sql, const Param* params, int param_count) {
  HSTMT hstmt{};
  SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, load_handle<HDBC>(hdbc_), &hstmt);
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw_diag(SQL_HANDLE_DBC, load_handle<HDBC>(hdbc_), "SQLAllocHandle(SQL_HANDLE_STMT)");
  }

  rc = SQLPrepare(hstmt, (SQLCHAR*)sql.data(), static_cast<SQLINTEGER>(sql.size()));
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    std::string msg = diag_message(SQL_HANDLE_STMT, hstmt);
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    throw std::runtime_error("SQLPrepare failed: " + msg);
  }

  // Storage for bound values and indicators to keep memory alive
  std::vector<int32_t> i32_vals;
  std::vector<int64_t> i64_vals;
  std::vector<double> dbl_vals;
  std::vector<std::string> str_vals;
  std::vector<SQLLEN> ind_vals;
  i32_vals.reserve(param_count);
  i64_vals.reserve(param_count);
  dbl_vals.reserve(param_count);
  str_vals.reserve(param_count);
  ind_vals.resize(param_count, 0);

  for (int i = 0; i < param_count; ++i) {
    SQLUSMALLINT paramNum = static_cast<SQLUSMALLINT>(i + 1);
    SQLSMALLINT cType = 0;
    SQLSMALLINT sqlType = 0;
    SQLULEN colDef = 0;
    SQLSMALLINT scale = 0;
    SQLPOINTER valPtr = nullptr;
    SQLLEN* indPtr = &ind_vals[i];

    const auto& v = params[i].value;
    if (std::holds_alternative<std::nullptr_t>(v)) {
      // Bind NULL, treat as VARCHAR with NULL indicator
      cType = SQL_C_CHAR;
      sqlType = SQL_VARCHAR;
      colDef = 1;
      scale = 0;
      valPtr = nullptr;
      *indPtr = SQL_NULL_DATA;
    } else if (auto pv = std::get_if<int32_t>(&v)) {
      cType = SQL_C_SLONG;
      sqlType = SQL_INTEGER;
      colDef = 0;
      scale = 0;
      i32_vals.push_back(*pv);
      valPtr = &i32_vals.back();
      *indPtr = sizeof(int32_t);
    } else if (auto pv = std::get_if<int64_t>(&v)) {
      cType = SQL_C_SBIGINT;
      sqlType = SQL_BIGINT;
      colDef = 0;
      scale = 0;
      i64_vals.push_back(*pv);
      valPtr = &i64_vals.back();
      *indPtr = sizeof(int64_t);
    } else if (auto pv = std::get_if<double>(&v)) {
      cType = SQL_C_DOUBLE;
      sqlType = SQL_DOUBLE;
      colDef = 0;
      scale = 0;
      dbl_vals.push_back(*pv);
      valPtr = &dbl_vals.back();
      *indPtr = sizeof(double);
    } else if (auto pv = std::get_if<std::string>(&v)) {
      cType = SQL_C_CHAR;
      sqlType = SQL_VARCHAR;
      colDef = static_cast<SQLULEN>(pv->size() > 0 ? pv->size() : 1);
      scale = 0;
      str_vals.push_back(*pv);
      valPtr = (SQLPOINTER)str_vals.back().data();
      *indPtr = static_cast<SQLLEN>(str_vals.back().size());
    }

    rc = SQLBindParameter(hstmt, paramNum, SQL_PARAM_INPUT,
                          cType, sqlType, colDef, scale,
                          valPtr, 0, indPtr);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
      std::string msg = diag_message(SQL_HANDLE_STMT, hstmt);
      SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
      throw std::runtime_error("SQLBindParameter failed: " + msg);
    }
  }

  rc = SQLExecute(hstmt);
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    std::string msg = diag_message(SQL_HANDLE_STMT, hstmt);
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    throw std::runtime_error("SQLExecute failed: " + msg);
  }

  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
}

void Connection::query_to_callback(std::string_view sql, const Param* params, int param_count,
                                   const std::function<void(const Row&)>& on_row) {
  std::scoped_lock lk(mtx_);
  ensure_connected_locked();

  HSTMT hstmt{};
  SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, load_handle<HDBC>(hdbc_), &hstmt);
  if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
    throw_diag(SQL_HANDLE_DBC, load_handle<HDBC>(hdbc_), "SQLAllocHandle(SQL_HANDLE_STMT)");
  }

  bool prepared = (params != nullptr && param_count > 0);
  if (prepared) {
    rc = SQLPrepare(hstmt, (SQLCHAR*)sql.data(), static_cast<SQLINTEGER>(sql.size()));
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
      std::string msg = diag_message(SQL_HANDLE_STMT, hstmt);
      SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
      throw std::runtime_error("SQLPrepare failed: " + msg);
    }

    // Bind parameters (reuse helper logic inline)
    std::vector<int32_t> i32_vals;
    std::vector<int64_t> i64_vals;
    std::vector<double> dbl_vals;
    std::vector<std::string> str_vals;
    std::vector<SQLLEN> ind_vals(param_count, 0);

    for (int i = 0; i < param_count; ++i) {
      SQLUSMALLINT paramNum = static_cast<SQLUSMALLINT>(i + 1);
      SQLSMALLINT cType = 0;
      SQLSMALLINT sqlType = 0;
      SQLULEN colDef = 0;
      SQLSMALLINT scale = 0;
      SQLPOINTER valPtr = nullptr;
      SQLLEN* indPtr = &ind_vals[i];

      const auto& v = params[i].value;
      if (std::holds_alternative<std::nullptr_t>(v)) {
        cType = SQL_C_CHAR;
        sqlType = SQL_VARCHAR;
        colDef = 1;
        scale = 0;
        valPtr = nullptr;
        *indPtr = SQL_NULL_DATA;
      } else if (auto pv = std::get_if<int32_t>(&v)) {
        cType = SQL_C_SLONG;
        sqlType = SQL_INTEGER;
        colDef = 0;
        scale = 0;
        i32_vals.push_back(*pv);
        valPtr = &i32_vals.back();
        *indPtr = sizeof(int32_t);
      } else if (auto pv = std::get_if<int64_t>(&v)) {
        cType = SQL_C_SBIGINT;
        sqlType = SQL_BIGINT;
        colDef = 0;
        scale = 0;
        i64_vals.push_back(*pv);
        valPtr = &i64_vals.back();
        *indPtr = sizeof(int64_t);
      } else if (auto pv = std::get_if<double>(&v)) {
        cType = SQL_C_DOUBLE;
        sqlType = SQL_DOUBLE;
        colDef = 0;
        scale = 0;
        dbl_vals.push_back(*pv);
        valPtr = &dbl_vals.back();
        *indPtr = sizeof(double);
      } else if (auto pv = std::get_if<std::string>(&v)) {
        cType = SQL_C_CHAR;
        sqlType = SQL_VARCHAR;
        colDef = static_cast<SQLULEN>(pv->size() > 0 ? pv->size() : 1);
        scale = 0;
        str_vals.push_back(*pv);
        valPtr = (SQLPOINTER)str_vals.back().data();
        *indPtr = static_cast<SQLLEN>(str_vals.back().size());
      }

      rc = SQLBindParameter(hstmt, paramNum, SQL_PARAM_INPUT,
                            cType, sqlType, colDef, scale,
                            valPtr, 0, indPtr);
      if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
        std::string msg = diag_message(SQL_HANDLE_STMT, hstmt);
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        throw std::runtime_error("SQLBindParameter failed: " + msg);
      }
    }

    rc = SQLExecute(hstmt);
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
      std::string msg = diag_message(SQL_HANDLE_STMT, hstmt);
      SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
      throw std::runtime_error("SQLExecute failed: " + msg);
    }
  } else {
    rc = SQLExecDirect(hstmt, (SQLCHAR*)sql.data(), static_cast<SQLINTEGER>(sql.size()));
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
      std::string msg = diag_message(SQL_HANDLE_STMT, hstmt);
      SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
      throw std::runtime_error("SQLExecDirect failed: " + msg);
    }
  }

  while (true) {
    rc = SQLFetch(hstmt);
    if (rc == SQL_NO_DATA) break;
    if (!(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)) {
      std::string msg = diag_message(SQL_HANDLE_STMT, hstmt);
      SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
      throw std::runtime_error("SQLFetch failed: " + msg);
    }
    Row row{store_handle(hstmt)};
    on_row(row);
  }

  SQLCloseCursor(hstmt);
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
}

// ---------------- Row getters ----------------

std::optional<int32_t> Connection::Row::getInt32(int col) const {
  SQLLEN ind = 0;
  int32_t val = 0;
  SQLRETURN rc = SQLGetData(load_handle<HSTMT>(hstmt_), static_cast<SQLUSMALLINT>(col), SQL_C_SLONG, &val, 0, &ind);
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
  SQLRETURN rc = SQLGetData(load_handle<HSTMT>(hstmt_), static_cast<SQLUSMALLINT>(col), SQL_C_SBIGINT, &val, 0, &ind);
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
  SQLRETURN rc = SQLGetData(load_handle<HSTMT>(hstmt_), static_cast<SQLUSMALLINT>(col), SQL_C_DOUBLE, &val, 0, &ind);
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
