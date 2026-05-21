#pragma once

#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <prometheus/summary.h>
#include <memory>

class CallDataMetrics {
public:
    explicit CallDataMetrics(const std::shared_ptr<prometheus::Registry>& registry);

    prometheus::Family<prometheus::Counter>& request_counter_family;
    prometheus::Family<prometheus::Histogram>& duration_histogram_family;
    prometheus::Family<prometheus::Summary>& duration_summary_family;
    prometheus::Family<prometheus::Histogram>& processing_histogram_family;
};
