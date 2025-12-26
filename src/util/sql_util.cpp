#include "sql_util.h"

std::string sql::anonymize(const std::string& namedSql) {
    std::string result;
    result.reserve(namedSql.size());

    enum State {
        NORMAL,
        IN_SINGLE_QUOTE,
        IN_DOUBLE_QUOTE,
        IN_LINE_COMMENT,
        IN_BLOCK_COMMENT,
        IN_NAMED_PARAM
    };

    State state = NORMAL;

    for (size_t i = 0; i < namedSql.length(); ++i) {
        char c = namedSql[i];
        char next = (i + 1 < namedSql.length()) ? namedSql[i + 1] : '\0';

        switch (state) {
        case NORMAL:
            if (c == '\'') {
                state = IN_SINGLE_QUOTE;
                result += c;
            } else if (c == '"') {
                state = IN_DOUBLE_QUOTE;
                result += c;
            } else if (c == '-' && next == '-') {
                state = IN_LINE_COMMENT;
                result += c;
                result += next;
                ++i;
            } else if (c == '/' && next == '*') {
                state = IN_BLOCK_COMMENT;
                result += c;
                result += next;
                ++i;
            } else if (c == ':') {
                if (next == ':') {
                    // Handle PostgreSQL-style cast ::
                    result += c;
                    result += next;
                    ++i;
                } else if (std::isalnum(static_cast<unsigned char>(next)) || next == '_') {
                    state = IN_NAMED_PARAM;
                    result += '?';
                } else {
                    result += c;
                }
            } else {
                result += c;
            }
            break;

        case IN_SINGLE_QUOTE:
            result += c;
            if (c == '\'') {
                if (next == '\'') {
                    // Escaped single quote in SQL
                    result += next;
                    ++i;
                } else {
                    state = NORMAL;
                }
            }
            break;

        case IN_DOUBLE_QUOTE:
            result += c;
            if (c == '"') {
                if (next == '"') {
                    // Escaped double quote
                    result += next;
                    ++i;
                } else {
                    state = NORMAL;
                }
            }
            break;

        case IN_LINE_COMMENT:
            result += c;
            if (c == '\n') {
                state = NORMAL;
            }
            break;

        case IN_BLOCK_COMMENT:
            result += c;
            if (c == '*' && next == '/') {
                result += next;
                ++i;
                state = NORMAL;
            }
            break;

        case IN_NAMED_PARAM:
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                state = NORMAL;
                // Re-process this character in NORMAL state
                --i;
            }
            // If it's part of the parameter name, we just don't add it to result
            break;
        }
    }

    return result;
}
