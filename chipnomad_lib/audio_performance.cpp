#include "audio_performance.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace audioPerformance {
thread_local bool sampling = false;
static std::atomic<uint32_t> requested{0};
static std::atomic<int> published{-1};
struct Slot {
  Snapshot data;
  std::atomic<uint32_t> state{0};
};
static Slot slots[3];
struct Writer {
  uint32_t generation = 0;
  uint32_t callbacks = 0, sampled = 0, overBudget = 0, sequence = 0;
  uint64_t budget = 0, detailBudget = 0;
  uint64_t current[keyCount]{}, sum[keyCount]{};
  uint32_t peak[keyCount]{};
};
static thread_local Writer writer;

uint64_t now() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}
void setEnabled(bool enabled) {
  uint32_t value = requested.load(std::memory_order_relaxed);
  if ((value & 1u) != (uint32_t)enabled)
    requested.store(value + 1u, std::memory_order_release);
}
void reset() {
  requested.fetch_add(2u, std::memory_order_release);
}
void add(int key, uint64_t elapsed) {
  if (key >= 0 && key < keyCount) writer.current[key] += elapsed;
}
static uint32_t load(uint64_t elapsed, uint64_t budget) {
  return budget ? (uint32_t)std::min<uint64_t>(elapsed * 1000u / budget, 999999u) : 0;
}
Callback::Callback(int frames, int rate, uint64_t started)
  : started_(started), budget_(0), enabled_(false) {
  sampling = false;
  const uint32_t generation = requested.load(std::memory_order_acquire);
  if (!(generation & 1u) || frames <= 0 || rate <= 0) return;
  enabled_ = true;
  budget_ = (uint64_t)frames * 1000000000u / rate;
  if (generation != writer.generation) {
    writer = Writer{};
    writer.generation = generation;
  }
  sampling = (writer.sequence++ % 4u) == 0;
  if (sampling) std::memset(writer.current, 0, sizeof(writer.current));
}
Callback::~Callback() {
  if (!enabled_) return;
  const uint64_t elapsed = now() - started_;
  writer.sum[callback] += elapsed;
  writer.budget += budget_;
  writer.peak[callback] = std::max(writer.peak[callback], load(elapsed, budget_));
  if (elapsed > budget_ && writer.overBudget != std::numeric_limits<uint32_t>::max()) ++writer.overBudget;
  if (sampling) {
    writer.detailBudget += budget_;
    ++writer.sampled;
    for (int key = 1; key < keyCount; ++key) {
      writer.sum[key] += writer.current[key];
      writer.peak[key] = std::max(writer.peak[key], load(writer.current[key], budget_));
    }
  }
  sampling = false;
  ++writer.callbacks;
  if (writer.budget < 250000000u) return;
  int previous = published.exchange(-1, std::memory_order_acq_rel);
  if (previous >= 0) {
    uint32_t expected = 1;
    slots[previous].state.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
  }
  for (int slot = 0; slot < 3; ++slot) {
    uint32_t expected = 0;
    if (!slots[slot].state.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) continue;
    auto& data = slots[slot].data;
    data.generation = writer.generation;
    data.callbacks = writer.callbacks;
    data.sampled = writer.sampled;
    data.overBudget = writer.overBudget;
    for (int key = 0; key < keyCount; ++key) {
      data.average[key] = load(writer.sum[key], key == callback ? writer.budget : writer.detailBudget);
      data.peak[key] = writer.peak[key];
    }
    slots[slot].state.store(1, std::memory_order_release);
    published.store(slot, std::memory_order_release);
    break;
  }
  writer.budget = writer.detailBudget = 0;
  writer.callbacks = writer.sampled = 0;
  std::memset(writer.sum, 0, sizeof(writer.sum));
}
bool receive(Snapshot& result) {
  int slot = published.load(std::memory_order_acquire);
  if (slot < 0) return false;
  uint32_t expected = 1;
  if (!slots[slot].state.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) return false;
  int value = slot;
  published.compare_exchange_strong(value, -1, std::memory_order_acq_rel);
  const bool fresh = slots[slot].data.generation == requested.load(std::memory_order_acquire);
  if (fresh) result = slots[slot].data;
  slots[slot].state.store(0, std::memory_order_release);
  return fresh;
}
}
