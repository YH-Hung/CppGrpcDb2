#include "cq_worker_metrics.h"

#include <vector>

namespace {

const std::vector<double>& DispatchDurationBuckets() {
    static const std::vector<double> buckets{
        0.0001, 0.0005, 0.001, 0.005, 0.01, 0.025,
        0.05,   0.1,    0.25,  0.5,   1.0};
    return buckets;
}

}  // namespace

CqWorkerMetrics::CqWorkerMetrics(const std::shared_ptr<prometheus::Registry>& registry)
    : busy_family_(prometheus::BuildGauge()
                       .Name("grpc_cq_worker_busy")
                       .Help("1 if the CQ worker thread is executing CallData::Proceed(), 0 if idle")
                       .Register(*registry)),
      busy_seconds_family_(prometheus::BuildCounter()
                               .Name("grpc_cq_worker_busy_seconds_total")
                               .Help("Cumulative seconds spent executing completion-queue tags")
                               .Register(*registry)),
      events_family_(prometheus::BuildCounter()
                         .Name("grpc_cq_worker_events_total")
                         .Help("Total number of completion-queue events dispatched")
                         .Register(*registry)),
      dispatch_duration_family_(prometheus::BuildHistogram()
                                    .Name("grpc_cq_worker_dispatch_duration_seconds")
                                    .Help("Wall-clock duration of each completion-queue dispatch")
                                    .Register(*registry)) {
    busy_gauge_ = &busy_family_.Add({});
    busy_seconds_counter_ = &busy_seconds_family_.Add({});
    events_ok_true_counter_ = &events_family_.Add({{"ok", "true"}});
    events_ok_false_counter_ = &events_family_.Add({{"ok", "false"}});
    dispatch_duration_ok_true_histogram_ =
        &dispatch_duration_family_.Add({{"ok", "true"}}, DispatchDurationBuckets());
    dispatch_duration_ok_false_histogram_ =
        &dispatch_duration_family_.Add({{"ok", "false"}}, DispatchDurationBuckets());
    SetBusy(false);
}

void CqWorkerMetrics::SetBusy(bool busy) {
    busy_gauge_->Set(busy ? 1.0 : 0.0);
}

void CqWorkerMetrics::ObserveDispatch(double seconds, bool ok) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }

    busy_seconds_counter_->Increment(seconds);

    auto* events_counter = ok ? events_ok_true_counter_ : events_ok_false_counter_;
    events_counter->Increment();

    auto* dispatch_duration_histogram =
        ok ? dispatch_duration_ok_true_histogram_ : dispatch_duration_ok_false_histogram_;
    dispatch_duration_histogram->Observe(seconds);
}

double CqWorkerMetrics::Busy() const {
    return busy_gauge_->Value();
}

double CqWorkerMetrics::BusySecondsTotal() const {
    return busy_seconds_counter_->Value();
}

double CqWorkerMetrics::EventsTotal(bool ok) const {
    return ok ? events_ok_true_counter_->Value() : events_ok_false_counter_->Value();
}
