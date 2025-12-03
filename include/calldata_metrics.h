#pragma once

#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <memory>

// Forward declare to avoid template exposure in header
namespace prometheus {
    template<typename T> class Family;
}

struct CallDataSharedMetrics {
    prometheus::Family<prometheus::Counter>* request_counter_family{nullptr};
    prometheus::Family<prometheus::Histogram>* duration_histogram_family{nullptr};
    prometheus::Family<prometheus::Histogram>* processing_histogram_family{nullptr};
    prometheus::Family<prometheus::Histogram>* request_size_histogram_family{nullptr};
    prometheus::Family<prometheus::Histogram>* response_size_histogram_family{nullptr};
};

class CallDataMetrics {
public:
    explicit CallDataMetrics(const std::shared_ptr<prometheus::Registry>& registry);
    CallDataSharedMetrics GetSharedMetrics() const { return shared_metrics_; }

private:
    CallDataSharedMetrics shared_metrics_{};
};
