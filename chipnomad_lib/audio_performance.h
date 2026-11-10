#pragma once
#include <atomic>
#include <cstdint>
#include "project_constants.h"

namespace audioPerformance {
enum Key {
  callback, voices, sequencer, trackEQ, inserts, delay, reverb,
  masterInsert1, masterInsert2, masterEQ, output,
  trackVoiceBase,
  trackEQBase = trackVoiceBase + PROJECT_MAX_TRACKS,
  trackInsertBase = trackEQBase + PROJECT_MAX_TRACKS,
  instrumentBase = trackInsertBase + PROJECT_MAX_TRACKS * 2,
  keyCount = instrumentBase + PROJECT_MAX_INSTRUMENTS
};
struct Snapshot {
  uint32_t generation = 0;
  uint32_t average[keyCount]{};
  uint32_t peak[keyCount]{};
  uint32_t overBudget = 0;
  uint32_t callbacks = 0;
  uint32_t sampled = 0;
};
static_assert(std::atomic<uint32_t>::is_always_lock_free, "Performance handoff must be lock-free");
void setEnabled(bool enabled);
void reset();
bool receive(Snapshot& result);
uint64_t now();
extern thread_local bool sampling;
void add(int key, uint64_t nanoseconds);

class Span {
 public:
  explicit Span(int key, int second = -1, int third = -1)
    : key_(sampling ? key : -1), second_(second), third_(third), start_(key_ >= 0 ? now() : 0) {}
  ~Span() {
    if (key_ < 0) return;
    const uint64_t elapsed = now() - start_;
    add(key_, elapsed);
    add(second_, elapsed);
    add(third_, elapsed);
  }
  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;
 private:
  int key_, second_, third_;
  uint64_t start_;
};

class Callback {
 public:
  Callback(int frames, int rate, uint64_t started);
  ~Callback();
  Callback(const Callback&) = delete;
  Callback& operator=(const Callback&) = delete;
 private:
  uint64_t started_, budget_;
  bool enabled_;
};
}
