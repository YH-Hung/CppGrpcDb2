/**
 * @file trace_log_formatter.h
 * @brief Custom spdlog formatter that injects OpenTelemetry trace context into log messages
 *
 * This formatter automatically appends trace_id and span_id to all log messages when an active
 * OpenTelemetry span exists. This enables correlation between distributed traces and log entries.
 *
 * Usage:
 * @code
 * #include "tracing/trace_log_formatter.h"
 *
 * // Set the formatter on the default logger
 * auto formatter = std::make_unique<tracing::TraceLogFormatter>();
 * spdlog::set_formatter(std::move(formatter));
 *
 * // Or set on a specific logger
 * auto logger = spdlog::get("my_logger");
 * logger->set_formatter(std::make_unique<tracing::TraceLogFormatter>());
 * @endcode
 *
 * Log Format:
 * [timestamp] [level] [trace_id=xxx] [span_id=xxx] message
 *
 * When no active span exists:
 * [timestamp] [level] message
 */

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/pattern_formatter.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/span_context.h>
#include <iomanip>
#include <sstream>

namespace tracing {

/**
 * @class TraceLogFormatter
 * @brief Custom spdlog formatter that appends OpenTelemetry trace context to log messages
 *
 * This formatter wraps the default pattern formatter and appends trace_id and span_id
 * from the current OpenTelemetry span context to each log message.
 *
 * Thread Safety: This formatter is thread-safe. OpenTelemetry's GetCurrentSpan() uses
 * thread-local storage, so each thread will extract its own trace context.
 *
 * Performance: Minimal overhead - only reads thread-local context and formats hex strings.
 */
class TraceLogFormatter : public spdlog::formatter {
public:
    /**
     * @brief Constructs a TraceLogFormatter
     * @param pattern Optional custom pattern (default: "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v")
     */
    explicit TraceLogFormatter(const std::string& pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v")
        : base_formatter_(std::make_unique<spdlog::pattern_formatter>(pattern)) {}

    /**
     * @brief Formats a log message with trace context
     * @param msg Log message details
     * @param dest Destination for formatted message
     *
     * Appends trace context in the format: [trace_id=xxx] [span_id=xxx]
     * If no active span exists, only the base message is formatted.
     */
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
        // Get trace context from OpenTelemetry
        std::string trace_context = get_trace_context();

        // Format base message first
        spdlog::memory_buf_t base_msg;
        base_formatter_->format(msg, base_msg);

        // The base formatter adds a newline at the end. We need to insert trace context before it.
        size_t base_size = base_msg.size();

        // Find the last newline character (if any)
        size_t insert_pos = base_size;
        if (base_size > 0 && base_msg.data()[base_size - 1] == '\n') {
            insert_pos = base_size - 1;
        }

        // Append trace context if available (insert before final newline)
        if (!trace_context.empty()) {
            // Append everything before the newline
            dest.append(base_msg.data(), base_msg.data() + insert_pos);
            // Append trace context
            dest.append(trace_context.data(), trace_context.data() + trace_context.size());
            // Append the newline
            if (insert_pos < base_size) {
                dest.push_back('\n');
            }
        } else {
            dest.append(base_msg.data(), base_msg.data() + base_msg.size());
        }
    }

    /**
     * @brief Creates a clone of this formatter
     * @return Unique pointer to cloned formatter
     */
    std::unique_ptr<spdlog::formatter> clone() const override {
        return std::make_unique<TraceLogFormatter>();
    }

private:
    std::unique_ptr<spdlog::formatter> base_formatter_;

    /**
     * @brief Extracts trace context from current OpenTelemetry span
     * @return Formatted trace context string, or empty string if no active span
     *
     * Format: " [trace_id=32charhex] [span_id=16charhex]"
     * The leading space ensures proper separation from the base message.
     */
    std::string get_trace_context() const {
        try {
            // Get the current span from OpenTelemetry context
            auto span = opentelemetry::trace::Tracer::GetCurrentSpan();
            if (!span) {
                return "";
            }

            auto span_context = span->GetContext();
            if (!span_context.IsValid()) {
                return "";
            }

            // Extract trace_id (128-bit) and span_id (64-bit)
            auto trace_id = span_context.trace_id();
            auto span_id = span_context.span_id();

            // Format as hex strings
            std::string trace_id_hex = format_trace_id(trace_id);
            std::string span_id_hex = format_span_id(span_id);

            // Build trace context string
            std::ostringstream oss;
            oss << " [trace_id=" << trace_id_hex << "] [span_id=" << span_id_hex << "]";
            return oss.str();

        } catch (const std::exception& e) {
            // Graceful degradation: if trace context extraction fails, log without trace info
            // Don't propagate exceptions from logging infrastructure
            return "";
        }
    }

    /**
     * @brief Formats a 128-bit trace ID as a 32-character hexadecimal string
     * @param trace_id OpenTelemetry trace ID
     * @return Hex string representation (lowercase, 32 characters)
     */
    static std::string format_trace_id(const opentelemetry::trace::TraceId& trace_id) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');

        // TraceId is 16 bytes (128 bits)
        auto id_data = trace_id.Id();
        for (size_t i = 0; i < opentelemetry::trace::TraceId::kSize; ++i) {
            oss << std::setw(2) << static_cast<unsigned int>(id_data[i]);
        }

        return oss.str();
    }

    /**
     * @brief Formats a 64-bit span ID as a 16-character hexadecimal string
     * @param span_id OpenTelemetry span ID
     * @return Hex string representation (lowercase, 16 characters)
     */
    static std::string format_span_id(const opentelemetry::trace::SpanId& span_id) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');

        // SpanId is 8 bytes (64 bits)
        auto id_data = span_id.Id();
        for (size_t i = 0; i < opentelemetry::trace::SpanId::kSize; ++i) {
            oss << std::setw(2) << static_cast<unsigned int>(id_data[i]);
        }

        return oss.str();
    }
};

/**
 * @brief Helper function to set trace-aware formatter on default logger
 *
 * Convenience function that configures the default spdlog logger with trace context injection.
 *
 * @code
 * tracing::SetTraceLogging();
 * spdlog::info("This message will include trace context if a span is active");
 * @endcode
 */
inline void SetTraceLogging() {
    spdlog::set_formatter(std::make_unique<TraceLogFormatter>());
}

/**
 * @brief Helper function to set trace-aware formatter on a specific logger
 * @param logger_name Name of the logger to configure
 *
 * @code
 * tracing::SetTraceLogging("my_logger");
 * spdlog::get("my_logger")->info("This message includes trace context");
 * @endcode
 */
inline void SetTraceLogging(const std::string& logger_name) {
    auto logger = spdlog::get(logger_name);
    if (logger) {
        logger->set_formatter(std::make_unique<TraceLogFormatter>());
    }
}

} // namespace tracing
