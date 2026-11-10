#pragma once
#include "../project_instruments.h"
#include "voice_post_processor.h"
#include <cstddef>
#include <cstdint>

class GranularVoice {
 public:
  ~GranularVoice();
  GranularVoice() = default;
  GranularVoice(const GranularVoice&) = delete;
  GranularVoice& operator=(const GranularVoice&) = delete;
  void init(float rate);
  bool reserve(size_t count);
  void configure(const InstrumentGranular& settings, float cents, float gain);
  void noteOn();
  void noteOff();
  void kill();
  void stopImmediately();
  void render(float* stereo, int frames);
  bool active() const { return playing_ || tail_ > 0; }
  float envelopeLevel() const { return post_.envelopeLevel(); }
  size_t capacity() const { return capacity_; }
  size_t grainCount() const { return count_; }
 private:
  struct Grain {
    double position, step;
    float phase, increment, left, right;
    uint32_t random;
  };
  Grain* grains_ = nullptr;
  size_t capacity_ = 0, count_ = 0;
  InstrumentGranular settings_{};
  VoicePostProcessor<> post_;
  float rate_ = 48000, cents_ = 0, fade_ = 0;
  double cursor_ = 0, baseStep_ = 1;
  bool playing_ = false;
  int tail_ = 0, tailLength_ = 144;
  float last_[2]{}, tailFrom_[2]{};
  float randomSigned(Grain& grain);
  void spawn(Grain& grain, double position);
};
