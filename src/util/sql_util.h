#ifndef CPPGRPCDB2_SQL_UTIL_H
#define CPPGRPCDB2_SQL_UTIL_H
#include <string>

namespace sql {
    // replace all named parameters (start with ':') with '?' in a SQL statement
    std::string anonymize(const std::string &namedSql);
}

#endif //CPPGRPCDB2_SQL_UTIL_H