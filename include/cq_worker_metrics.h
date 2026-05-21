#pragma once

#include <memory>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

class CqWorkerMetrics {
public:
    explicit CqWorkerMetrics(const std::shared_ptr<prometheus::Registry>& registry);

    void SetBusy(bool busy);
    void ObserveDispatch(double seconds, bool ok);

    double Busy() const;
    double BusySecondsTotal() const;
    double EventsTotal(bool ok) const;

private:
    prometheus::Family<prometheus::Gauge>& busy_family_;
    prometheus::Family<prometheus::Counter>& busy_seconds_family_;
    prometheus::Family<prometheus::Counter>& events_family_;
    prometheus::Family<prometheus::Histogram>& dispatch_duration_family_;

    prometheus::Gauge* busy_gauge_{nullptr};
    prometheus::Counter* busy_seconds_counter_{nullptr};
    prometheus::Counter* events_ok_true_counter_{nullptr};
    prometheus::Counter* events_ok_false_counter_{nullptr};
    prometheus::Histogram* dispatch_duration_ok_true_histogram_{nullptr};
    prometheus::Histogram* dispatch_duration_ok_false_histogram_{nullptr};
};
