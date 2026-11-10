#include "granular_voice.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
struct Window {
  float values[1025];
  Window() {
    for (int i = 0; i <= 1024; ++i)
      values[i] = 0.5f - 0.5f * std::cos(i * 6.283185307179586f / 1024);
  }
};
const Window window;
float envelopeSeconds(uint8_t value) { return value * value * 5.0f / 65025.0f; }
double wrap(double position, uint32_t frames) {
  position = std::fmod(position, (double)frames);
  return position < 0 ? position + frames : position;
}
}
GranularVoice::~GranularVoice() { std::free(grains_); }
void GranularVoice::init(float rate) {
  rate_ = rate > 0 ? rate : 48000;
  tailLength_ = std::max(1, (int)(rate_ * 0.003f));
  post_.init(rate_);
  stopImmediately();
}
bool GranularVoice::reserve(size_t count) {
  if (count <= capacity_) return true;
  if (count > std::numeric_limits<size_t>::max() / sizeof(Grain)) return false;
  void* memory = std::realloc(grains_, count * sizeof(Grain));
  if (!memory) return false;
  grains_ = static_cast<Grain*>(memory);
  for (size_t i = capacity_; i < count; ++i) grains_[i] = Grain{0, 0, 1, 1, 0, 0, 0x47524149u ^ ((uint32_t)(i + 1) * 0x9e3779b9u)};
  capacity_ = count;
  return true;
}
void GranularVoice::configure(const InstrumentGranular& settings, float cents, float gain) {
  if (settings.sample.data != settings_.sample.data || settings.sample.start != settings_.sample.start)
    cursor_ = sampleMarkerToStartFrame(settings.sample.frameCount, settings.sample.start);
  settings_ = settings;
  cents_ = cents;
  baseStep_ = (settings.sample.sampleRate > 0 ? settings.sample.sampleRate : rate_) / rate_ * std::exp2(cents_ / 1200.0f);
  const size_t next = std::min<size_t>(settings.grains, capacity_);
  for (size_t i = count_; i < next; ++i) {
    spawn(grains_[i], cursor_);
    grains_[i].phase = (float)i / std::max<size_t>(1, next);
  }
  count_ = next;
  post_.setGain(gain);
  const auto& p = settings.sample;
  post_.setFilter(p.filterEnabled != 0, p.filterCharacter, p.filterMode, p.filterSlope24dB != 0,
    p.filterCutoffHz, p.filterResonance / 255.0f, p.filterColor);
  post_.setEnvelope(true, envelopeSeconds(p.attack), envelopeSeconds(p.decay),
    p.sustain / 255.0f, envelopeSeconds(p.release), p.envelopeShape);
}
float GranularVoice::randomSigned(Grain& grain) {
  grain.random ^= grain.random << 13;
  grain.random ^= grain.random >> 17;
  grain.random ^= grain.random << 5;
  return (grain.random >> 8) / 8388607.5f - 1.0f;
}
void GranularVoice::spawn(Grain& grain, double position) {
  const auto& sample = settings_.sample;
  grain.position = sample.frameCount ? wrap(position + randomSigned(grain) *
    settings_.scatter / 255.0 * sample.frameCount * 0.5, sample.frameCount) : 0;
  const float pitch = randomSigned(grain) * settings_.pitchSpread;
  grain.step = pitch == 0 ? baseStep_ : baseStep_ * std::exp2(pitch / 1200.0f);
  grain.phase = 0;
  grain.increment = 1000.0f / (std::max<int>(1, settings_.sizeMs) * rate_);
  const float pan = randomSigned(grain) * settings_.width / 255.0f;
  grain.left = 1.0f - std::max(0.0f, pan);
  grain.right = 1.0f + std::min(0.0f, pan);
}
void GranularVoice::noteOn() {
  if (!settings_.sample.data || !settings_.sample.frameCount || !count_) return;
  cursor_ = sampleMarkerToStartFrame(settings_.sample.frameCount, settings_.sample.start);
  for (size_t i = 0; i < count_; ++i) {
    grains_[i].random = 0x47524149u ^ ((uint32_t)(i + 1) * 0x9e3779b9u);
    spawn(grains_[i], cursor_);
    grains_[i].phase = (float)i / count_;
  }
  playing_ = true;
  fade_ = 0;
  tail_ = 0;
  post_.noteOn(true);
}
void GranularVoice::noteOff() { post_.noteOff(); }
void GranularVoice::kill() {
  if (playing_) {
    tailFrom_[0] = last_[0]; tailFrom_[1] = last_[1];
    tail_ = tailLength_;
  }
  playing_ = false;
  post_.kill();
}
void GranularVoice::stopImmediately() {
  playing_ = false;
  tail_ = 0;
  last_[0] = last_[1] = 0;
  post_.kill();
}
void GranularVoice::render(float* stereo, int frames) {
  if (!stereo || frames <= 0) return;
  std::memset(stereo, 0, (size_t)frames * 2 * sizeof(float));
  const auto& sample = settings_.sample;
  if (playing_ && sample.data && sample.frameCount && count_) {
    const double scan = settings_.scanPercent / 100.0 *
      (sample.sampleRate > 0 ? sample.sampleRate : rate_) / rate_;
    const float normalization = 2.0f / count_;
    for (size_t g = 0; g < count_; ++g) {
      auto& grain = grains_[g];
      for (int i = 0; i < frames; ++i) {
        if (grain.phase >= 1) spawn(grain, cursor_ + i * scan);
        const uint32_t first = (uint32_t)grain.position;
        const uint32_t next = first + 1 < sample.frameCount ? first + 1 : 0;
        const float fraction = (float)(grain.position - first);
        const float phase = std::clamp(grain.phase * 1024, 0.0f, 1024.0f);
        const int w = std::min(1023, (int)phase);
        const float weight = (window.values[w] + (window.values[w + 1] - window.values[w]) * (phase - w)) * normalization;
        const int channels = sample.channels >= 2 ? 2 : 1;
        for (int c = 0; c < 2; ++c) {
          const int channel = channels == 1 ? 0 : c;
          const float a = sample.data[(size_t)first * channels + channel] / 32768.0f;
          const float b = sample.data[(size_t)next * channels + channel] / 32768.0f;
          stereo[i * 2 + c] += (a + (b - a) * fraction) * weight * (c ? grain.right : grain.left);
        }
        grain.phase += grain.increment;
        grain.position += grain.step;
        if (grain.position >= sample.frameCount) grain.position = wrap(grain.position, sample.frameCount);
      }
    }
    cursor_ = wrap(cursor_ + frames * scan, sample.frameCount);
  }
  for (int i = 0; i < frames; ++i) {
    if (playing_) {
      if (!post_.envelopeActive()) kill();
      else {
        post_.processStereo(stereo + i * 2);
        fade_ = std::min(1.0f, fade_ + 1.0f / std::max(1.0f, rate_ * 0.002f));
        for (int c = 0; c < 2; ++c) {
          stereo[i * 2 + c] *= fade_;
          last_[c] = stereo[i * 2 + c];
        }
      }
    }
    if (!playing_) {
      const float weight = tail_ > 0 ? (float)tail_-- / tailLength_ : 0;
      stereo[i * 2] = tailFrom_[0] * weight;
      stereo[i * 2 + 1] = tailFrom_[1] * weight;
    }
  }
}
