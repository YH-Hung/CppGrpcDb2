#include "cq_worker_metrics.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <prometheus/metric_family.h>
#include <prometheus/registry.h>

namespace {

const prometheus::MetricFamily& FindFamily(
    const std::vector<prometheus::MetricFamily>& families,
    const std::string& name) {
    for (const auto& family : families) {
        if (family.name == name) {
            return family;
        }
    }
    throw std::runtime_error("Metric family not found: " + name);
}

bool HasLabel(const prometheus::ClientMetric& metric,
              const std::string& name,
              const std::string& value) {
    for (const auto& label : metric.label) {
        if (label.name == name && label.value == value) {
            return true;
        }
    }
    return false;
}

const prometheus::ClientMetric& FindMetricByOkLabel(
    const prometheus::MetricFamily& family,
    const std::string& ok_value) {
    for (const auto& metric : family.metric) {
        if (HasLabel(metric, "ok", ok_value)) {
            return metric;
        }
    }
    throw std::runtime_error("Metric with ok label not found: " + ok_value);
}

}  // namespace

TEST(CqWorkerMetricsTest, SetBusyUpdatesGauge) {
    auto registry = std::make_shared<prometheus::Registry>();
    CqWorkerMetrics metrics(registry);

    metrics.SetBusy(true);

    auto families = registry->Collect();
    const auto& busy_family = FindFamily(families, "grpc_cq_worker_busy");
    ASSERT_EQ(busy_family.metric.size(), 1U);
    EXPECT_DOUBLE_EQ(busy_family.metric.front().gauge.value, 1.0);
    EXPECT_DOUBLE_EQ(metrics.Busy(), 1.0);

    metrics.SetBusy(false);

    families = registry->Collect();
    const auto& idle_family = FindFamily(families, "grpc_cq_worker_busy");
    ASSERT_EQ(idle_family.metric.size(), 1U);
    EXPECT_DOUBLE_EQ(idle_family.metric.front().gauge.value, 0.0);
    EXPECT_DOUBLE_EQ(metrics.Busy(), 0.0);
}

TEST(CqWorkerMetricsTest, ObserveDispatchAccumulatesBusySeconds) {
    auto registry = std::make_shared<prometheus::Registry>();
    CqWorkerMetrics metrics(registry);

    metrics.ObserveDispatch(0.25, true);
    metrics.ObserveDispatch(0.75, true);

    const auto families = registry->Collect();
    const auto& busy_seconds_family =
        FindFamily(families, "grpc_cq_worker_busy_seconds_total");

    ASSERT_EQ(busy_seconds_family.metric.size(), 1U);
    EXPECT_DOUBLE_EQ(busy_seconds_family.metric.front().counter.value, 1.0);
    EXPECT_DOUBLE_EQ(metrics.BusySecondsTotal(), 1.0);
}

TEST(CqWorkerMetricsTest, ObserveDispatchSeparatesEventsByOkLabel) {
    auto registry = std::make_shared<prometheus::Registry>();
    CqWorkerMetrics metrics(registry);

    metrics.ObserveDispatch(0.25, true);
    metrics.ObserveDispatch(0.75, false);
    metrics.ObserveDispatch(0.5, false);

    const auto families = registry->Collect();
    const auto& events_family = FindFamily(families, "grpc_cq_worker_events_total");
    const auto& ok_true_metric = FindMetricByOkLabel(events_family, "true");
    const auto& ok_false_metric = FindMetricByOkLabel(events_family, "false");

    EXPECT_DOUBLE_EQ(ok_true_metric.counter.value, 1.0);
    EXPECT_DOUBLE_EQ(ok_false_metric.counter.value, 2.0);
    EXPECT_DOUBLE_EQ(metrics.EventsTotal(true), 1.0);
    EXPECT_DOUBLE_EQ(metrics.EventsTotal(false), 2.0);
}

TEST(CqWorkerMetricsTest, ObserveDispatchRecordsHistogramCountAndSum) {
    auto registry = std::make_shared<prometheus::Registry>();
    CqWorkerMetrics metrics(registry);

    metrics.ObserveDispatch(0.25, true);
    metrics.ObserveDispatch(0.75, true);
    metrics.ObserveDispatch(0.5, false);

    const auto families = registry->Collect();
    const auto& histogram_family =
        FindFamily(families, "grpc_cq_worker_dispatch_duration_seconds");
    const auto& ok_true_metric = FindMetricByOkLabel(histogram_family, "true");
    const auto& ok_false_metric = FindMetricByOkLabel(histogram_family, "false");

    EXPECT_EQ(ok_true_metric.histogram.sample_count, 2U);
    EXPECT_DOUBLE_EQ(ok_true_metric.histogram.sample_sum, 1.0);
    EXPECT_FALSE(ok_true_metric.histogram.bucket.empty());

    EXPECT_EQ(ok_false_metric.histogram.sample_count, 1U);
    EXPECT_DOUBLE_EQ(ok_false_metric.histogram.sample_sum, 0.5);
    EXPECT_FALSE(ok_false_metric.histogram.bucket.empty());
}
