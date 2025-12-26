#include <gtest/gtest.h>
#include "util/sql_util.h"

TEST(SqlUtilTest, BasicAnonymization) {
    EXPECT_EQ(sql::anonymize("SELECT * FROM users WHERE id = :id"), 
              "SELECT * FROM users WHERE id = ?");
    EXPECT_EQ(sql::anonymize("UPDATE users SET name = :name, age = :age WHERE id = :id"), 
              "UPDATE users SET name = ?, age = ? WHERE id = ?");
}

TEST(SqlUtilTest, MultipleParameters) {
    EXPECT_EQ(sql::anonymize("INSERT INTO table VALUES (:v1, :v2, :v3)"), 
              "INSERT INTO table VALUES (?, ?, ?)");
}

TEST(SqlUtilTest, Strings) {
    // Parameters inside single quotes should not be anonymized
    EXPECT_EQ(sql::anonymize("SELECT ':not_a_param' FROM t"), 
              "SELECT ':not_a_param' FROM t");
    // Parameters inside double quotes should not be anonymized (common for identifiers)
    EXPECT_EQ(sql::anonymize("SELECT \":not_a_param\" FROM t"), 
              "SELECT \":not_a_param\" FROM t");
    // Escaped quotes in strings
    EXPECT_EQ(sql::anonymize("SELECT 'It''s a :not_a_param' FROM t"), 
              "SELECT 'It''s a :not_a_param' FROM t");
    EXPECT_EQ(sql::anonymize("SELECT \"a \"\":not_a_param\"\" b\" FROM t"), 
              "SELECT \"a \"\":not_a_param\"\" b\" FROM t");
}

TEST(SqlUtilTest, Comments) {
    // Line comments
    EXPECT_EQ(sql::anonymize("SELECT * FROM t -- comment with :param\nWHERE id = :id"), 
              "SELECT * FROM t -- comment with :param\nWHERE id = ?");
    // Block comments
    EXPECT_EQ(sql::anonymize("SELECT * FROM t /* block with :param */ WHERE id = :id"), 
              "SELECT * FROM t /* block with :param */ WHERE id = ?");
}

TEST(SqlUtilTest, EdgeCases) {
    // Colon at the end
    EXPECT_EQ(sql::anonymize("SELECT * FROM t WHERE id = :"), 
              "SELECT * FROM t WHERE id = :");
    // Double colon (PostgreSQL cast)
    EXPECT_EQ(sql::anonymize("SELECT val::text FROM t WHERE id = :id"), 
              "SELECT val::text FROM t WHERE id = ?");
    // Named param with underscores and numbers
    EXPECT_EQ(sql::anonymize("SELECT * FROM t WHERE user_id = :user_id123"), 
              "SELECT * FROM t WHERE user_id = ?");
    // No parameters
    EXPECT_EQ(sql::anonymize("SELECT 1"), "SELECT 1");
    // Empty string
    EXPECT_EQ(sql::anonymize(""), "");
    // Parameter at the very end
    EXPECT_EQ(sql::anonymize("SELECT * FROM t WHERE id = :id"), "SELECT * FROM t WHERE id = ?");
}

TEST(SqlUtilTest, MixedCases) {
    std::string sql = "/* prefix */ SELECT :a, ':b', \":c\", -- :d\n :e /* :f */";
    std::string expected = "/* prefix */ SELECT ?, ':b', \":c\", -- :d\n ? /* :f */";
    EXPECT_EQ(sql::anonymize(sql), expected);
}

TEST(SqlUtilTest, ManyParameters) {
    std::string sql = "INSERT INTO t VALUES (:p1, :p2, :p3, :p4, :p5, :p6, :p7, :p8, :p9, :p10, :p11, :p12)";
    std::string expected = "INSERT INTO t VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    EXPECT_EQ(sql::anonymize(sql), expected);
}

TEST(SqlUtilTest, SelectMixed) {
    std::string sql = "SELECT name, 'static_val', :param1, 123, :param2 FROM users WHERE status = 'active' AND id = :id";
    std::string expected = "SELECT name, 'static_val', ?, 123, ? FROM users WHERE status = 'active' AND id = ?";
    EXPECT_EQ(sql::anonymize(sql), expected);
}

TEST(SqlUtilTest, InsertMixed) {
    std::string sql = "INSERT INTO orders (id, customer_id, amount, status, created_at) VALUES (:id, :cust_id, 99.99, 'PENDING', NOW())";
    std::string expected = "INSERT INTO orders (id, customer_id, amount, status, created_at) VALUES (?, ?, 99.99, 'PENDING', NOW())";
    EXPECT_EQ(sql::anonymize(sql), expected);
}

TEST(SqlUtilTest, UpdateMixed) {
    std::string sql = "UPDATE products SET price = :new_price, updated_at = CURRENT_TIMESTAMP, stock = stock - 1 WHERE id = :prod_id AND category = 'electronics'";
    std::string expected = "UPDATE products SET price = ?, updated_at = CURRENT_TIMESTAMP, stock = stock - 1 WHERE id = ? AND category = 'electronics'";
    EXPECT_EQ(sql::anonymize(sql), expected);
}

TEST(SqlUtilTest, ComplexMonsterSql) {
    std::string sql = 
        "WITH RECURSIVE subordinates AS (\n"
        "    SELECT employee_id, manager_id, name, 1 as level\n"
        "    FROM employees\n"
        "    WHERE employee_id = :start_id -- starting point\n"
        "    UNION ALL\n"
        "    SELECT e.employee_id, e.manager_id, e.name, s.level + 1\n"
        "    FROM employees e\n"
        "    INNER JOIN subordinates s ON s.employee_id = e.manager_id\n"
        ")\n"
        "SELECT \n"
        "    s.name AS \"Employee Name\",\n"
        "    'Level: ' || s.level AS level_info,\n"
        "    (SELECT COUNT(*) FROM tasks t WHERE t.assignee_id = s.employee_id AND t.status != 'DONE') as pending_tasks,\n"
        "    :extra_param as \"Extra :param\"\n"
        "FROM subordinates s\n"
        "WHERE s.level <= :max_level\n"
        "  AND s.name NOT LIKE '%test%' /* ignore :test_users */\n"
        "  AND s.name != ':not_a_param'\n"
        "ORDER BY s.level, s.name;";

    std::string expected = 
        "WITH RECURSIVE subordinates AS (\n"
        "    SELECT employee_id, manager_id, name, 1 as level\n"
        "    FROM employees\n"
        "    WHERE employee_id = ? -- starting point\n"
        "    UNION ALL\n"
        "    SELECT e.employee_id, e.manager_id, e.name, s.level + 1\n"
        "    FROM employees e\n"
        "    INNER JOIN subordinates s ON s.employee_id = e.manager_id\n"
        ")\n"
        "SELECT \n"
        "    s.name AS \"Employee Name\",\n"
        "    'Level: ' || s.level AS level_info,\n"
        "    (SELECT COUNT(*) FROM tasks t WHERE t.assignee_id = s.employee_id AND t.status != 'DONE') as pending_tasks,\n"
        "    ? as \"Extra :param\"\n"
        "FROM subordinates s\n"
        "WHERE s.level <= ?\n"
        "  AND s.name NOT LIKE '%test%' /* ignore :test_users */\n"
        "  AND s.name != ':not_a_param'\n"
        "ORDER BY s.level, s.name;";

    EXPECT_EQ(sql::anonymize(sql), expected);
}

TEST(SqlUtilTest, NestedAndMixed) {
    std::string sql = 
        "SELECT * FROM (\n"
        "    SELECT :p1 as p1, ':p2' as p2, \":p3\" as p3, \n"
        "    'string with escaped '' quote and :p4' as p4,\n"
        "    \"identifier with \"\" quotes and :p5\" as p5,\n"
        "    /* block comment with \n"
        "       multiple lines and :p6 */\n"
        "    -- line comment with :p7\n"
        "    :p8 as p8\n"
        ") t WHERE t.p1 = :p1_val AND t.p8::integer > :min_val";

    std::string expected = 
        "SELECT * FROM (\n"
        "    SELECT ? as p1, ':p2' as p2, \":p3\" as p3, \n"
        "    'string with escaped '' quote and :p4' as p4,\n"
        "    \"identifier with \"\" quotes and :p5\" as p5,\n"
        "    /* block comment with \n"
        "       multiple lines and :p6 */\n"
        "    -- line comment with :p7\n"
        "    ? as p8\n"
        ") t WHERE t.p1 = ? AND t.p8::integer > ?";

    EXPECT_EQ(sql::anonymize(sql), expected);
}

TEST(SqlUtilTest, DeeplyNestedAndDiverse) {
    std::string sql = 
        "SELECT CASE WHEN :cond1 THEN (SELECT :p1 FROM t1 WHERE c = ':not_p') ELSE :p2 END "
        "FROM (SELECT * FROM t2 WHERE id IN (SELECT id FROM t3 WHERE x = :p3)) AS sub "
        "WHERE sub.col = \"col:with:colon\" AND sub.val > :p4";
    std::string expected = 
        "SELECT CASE WHEN ? THEN (SELECT ? FROM t1 WHERE c = ':not_p') ELSE ? END "
        "FROM (SELECT * FROM t2 WHERE id IN (SELECT id FROM t3 WHERE x = ?)) AS sub "
        "WHERE sub.col = \"col:with:colon\" AND sub.val > ?";
    EXPECT_EQ(sql::anonymize(sql), expected);
}
