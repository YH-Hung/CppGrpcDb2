#include <gtest/gtest.h>
#include "db2/db2.hpp"
#include <cstdlib>
#include <thread>
#include <future>

// These are integration-style tests. They will be skipped if DB2_CONN_STR is not set.
// Set DB2_CONN_STR to a valid DB2 CLI connection string, e.g.:
//   DATABASE=db;HOSTNAME=host;PORT=50000;PROTOCOL=TCPIP;UID=user;PWD=pass;

static bool has_conn_str() {
  const char* cs = std::getenv("DB2_CONN_STR");
  return cs && *cs;
}

static std::string conn_str() {
  const char* cs = std::getenv("DB2_CONN_STR");
  return cs ? std::string(cs) : std::string();
}

TEST(Db2Wrapper, ConnectAndSimpleExec) {
  if (!has_conn_str()) GTEST_SKIP() << "DB2_CONN_STR not set";
  db2::Connection c;
  c.connect_with_conn_str(conn_str());
  EXPECT_TRUE(c.is_connected());
  // Lightweight statement
  c.execute("VALUES 1");
}

TEST(Db2Wrapper, VeryLongSqlString) {
  if (!has_conn_str()) GTEST_SKIP() << "DB2_CONN_STR not set";
  db2::Connection c;
  c.connect_with_conn_str(conn_str());
  // Create a very long SQL by padding spaces after VALUES
  std::string sql = "VALUES 1 ";
  sql.reserve(200000);
  while (sql.size() < 150000) sql += ' ';
  c.execute(sql);
}

TEST(Db2Wrapper, NullParamBinding) {
  if (!has_conn_str()) GTEST_SKIP() << "DB2_CONN_STR not set";
  db2::Connection c;
  c.connect_with_conn_str(conn_str());
  // Prepare-like usage with NULL parameter in a trivial CAST
  std::vector<db2::Param> params;
  params.emplace_back(nullptr);
  // DB2: SELECT CAST(? AS VARCHAR(1)) is allowed
  c.execute("SELECT CAST(? AS VARCHAR(1)) FROM SYSIBM.SYSDUMMY1", params);
}

TEST(Db2Wrapper, ConcurrentAccess) {
  if (!has_conn_str()) GTEST_SKIP() << "DB2_CONN_STR not set";
  db2::Connection c;
  c.connect_with_conn_str(conn_str());

  auto worker = [&c](int n){
    for (int i = 0; i < n; ++i) {
      c.execute("VALUES 1");
      auto rows = c.query<int>(
          "SELECT 1 FROM SYSIBM.SYSDUMMY1",
          [](const db2::Connection::Row& r){ return 1; }
      );
      ASSERT_FALSE(rows.empty());
    }
  };

  std::thread t1(worker, 5);
  std::thread t2(worker, 5);
  t1.join();
  t2.join();
}
