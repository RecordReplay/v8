// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_LOGGING_COUNTERS_SCOPES_H_
#define V8_LOGGING_COUNTERS_SCOPES_H_

#include "src/execution/isolate.h"
#include "src/logging/counters.h"
#include "src/logging/log.h"

#include "replayio.h"

namespace v8 {
namespace internal {

class BaseTimedHistogramScope {
 protected:
  explicit BaseTimedHistogramScope(TimedHistogram* histogram)
      : histogram_(histogram) {}

  void StartInternal() {
    DCHECK(histogram_->ToggleRunningState(true));
    timer_.Start();
  }

  void StopInternal() {
    DCHECK(histogram_->ToggleRunningState(false));
    histogram_->AddTimedSample(timer_.Elapsed());
    timer_.Stop();
  }

  V8_INLINE void Start() {
    if (histogram_->Enabled()) StartInternal();
  }

  V8_INLINE void Stop() {
    if (histogram_->Enabled()) StopInternal();
  }

  V8_INLINE void LogStart(Isolate* isolate) {
    V8FileLogger::CallEventLogger(isolate, histogram_->name(),
                                  v8::LogEventStatus::kStart, true);
  }

  V8_INLINE void LogEnd(Isolate* isolate) {
    V8FileLogger::CallEventLogger(isolate, histogram_->name(),
                                  v8::LogEventStatus::kEnd, true);
  }

  base::ElapsedTimer timer_;
  TimedHistogram* histogram_;
};

// Helper class for scoping a TimedHistogram.
class V8_NODISCARD TimedHistogramScope : public BaseTimedHistogramScope {
 public:
  explicit TimedHistogramScope(TimedHistogram* histogram,
                               Isolate* isolate = nullptr)
      : BaseTimedHistogramScope(histogram), isolate_(isolate) {
    Start();
    if (isolate_) LogStart(isolate_);
  }

  ~TimedHistogramScope() {
    Stop();
    if (isolate_) LogEnd(isolate_);
  }

 private:
  Isolate* const isolate_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(TimedHistogramScope);
};

enum class OptionalTimedHistogramScopeMode { TAKE_TIME, DONT_TAKE_TIME };

// Helper class for scoping a TimedHistogram.
// It will not take time for mode = DONT_TAKE_TIME.
class V8_NODISCARD OptionalTimedHistogramScope
    : public BaseTimedHistogramScope {
 public:
  OptionalTimedHistogramScope(TimedHistogram* histogram, Isolate* isolate,
                              OptionalTimedHistogramScopeMode mode)
      : BaseTimedHistogramScope(histogram), isolate_(isolate), mode_(mode) {
    if (mode != OptionalTimedHistogramScopeMode::TAKE_TIME) return;
    Start();
    LogStart(isolate_);
  }

  ~OptionalTimedHistogramScope() {
    if (mode_ != OptionalTimedHistogramScopeMode::TAKE_TIME) return;
    Stop();
    LogEnd(isolate_);
  }

 private:
  Isolate* const isolate_;
  const OptionalTimedHistogramScopeMode mode_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(OptionalTimedHistogramScope);
};

// Helper class for scoping a TimedHistogram, where the histogram is selected at
// stop time rather than start time.
class V8_NODISCARD LazyTimedHistogramScope : public BaseTimedHistogramScope {
 public:
  LazyTimedHistogramScope() : BaseTimedHistogramScope(nullptr) {
    timer_.Start();
  }
  ~LazyTimedHistogramScope() {
    // We should set the histogram before this scope exits.
    Stop();
  }

  void set_histogram(TimedHistogram* histogram) {
    DCHECK_IMPLIES(histogram->Enabled(), histogram->ToggleRunningState(true));
    histogram_ = histogram;
  }
};

// Helper class for scoping a NestedHistogramTimer.
class V8_NODISCARD NestedTimedHistogramScope : public BaseTimedHistogramScope {
 public:
  explicit NestedTimedHistogramScope(NestedTimedHistogram* histogram,
                                     Isolate* isolate = nullptr)
      : BaseTimedHistogramScope(histogram), isolate_(isolate) {
    Start();
  }
  ~NestedTimedHistogramScope() { Stop(); }

 private:
  friend NestedTimedHistogram;
  friend PauseNestedTimedHistogramScope;

  void StartInteral() {
    replayio::AutoPassThroughEvents pt;

    previous_scope_ = timed_histogram()->Enter(this);
    base::TimeTicks now = base::TimeTicks::Now();
    if (previous_scope_) previous_scope_->Pause(now);
    timer_.Start(now);
  }

  void StopInternal() {
    replayio::AutoPassThroughEvents pt;

    timed_histogram()->Leave(previous_scope_);
    base::TimeTicks now = base::TimeTicks::Now();
    base::TimeDelta elapsed = timer_.Elapsed(now);
    histogram_->AddTimedSample(elapsed);
    if (isolate_) RecordLongTaskTime(elapsed);
#ifdef DEBUG
    // StopInternal() is called in the destructor and don't access timer_
    // after that.
    timer_.Stop();
#endif
    if (previous_scope_) previous_scope_->Resume(now);
  }

  V8_INLINE void Start() {
    if (histogram_->Enabled()) StartInteral();
    LogStart(timed_histogram()->counters()->isolate());
  }

  V8_INLINE void Stop() {
    if (histogram_->Enabled()) StopInternal();
    LogEnd(timed_histogram()->counters()->isolate());
  }

  void Pause(base::TimeTicks now) {
    DCHECK(histogram_->Enabled());
    timer_.Pause(now);
  }

  void Resume(base::TimeTicks now) {
    DCHECK(histogram_->Enabled());
    timer_.Resume(now);
  }

  void RecordLongTaskTime(base::TimeDelta elapsed) const {
    if (histogram_ == isolate_->counters()->execute()) {
      isolate_->GetCurrentLongTaskStats()->v8_execute_us +=
          elapsed.InMicroseconds();
    }
  }

  NestedTimedHistogram* timed_histogram() {
    return static_cast<NestedTimedHistogram*>(histogram_);
  }

  NestedTimedHistogramScope* previous_scope_;
  Isolate* isolate_;
};

// Temporarily pause a NestedTimedHistogram when for instance leaving V8 for
// external callbacks.
class V8_NODISCARD PauseNestedTimedHistogramScope {
 public:
  explicit PauseNestedTimedHistogramScope(NestedTimedHistogram* histogram)
      : histogram_(histogram) {
    previous_scope_ = histogram_->Enter(nullptr);
    if (isEnabled()) {
      previous_scope_->Pause(base::TimeTicks::Now());
    }
  }
  ~PauseNestedTimedHistogramScope() {
    histogram_->Leave(previous_scope_);
    if (isEnabled()) {
      previous_scope_->Resume(base::TimeTicks::Now());
    }
  }

 private:
  bool isEnabled() const { return previous_scope_ && histogram_->Enabled(); }
  NestedTimedHistogram* histogram_;
  NestedTimedHistogramScope* previous_scope_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_LOGGING_COUNTERS_SCOPES_H_
