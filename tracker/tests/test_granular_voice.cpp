#include "doctest.h"
#include "synth/granular_voice.h"
#include "chipnomad_lib.h"
#include "project.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
InstrumentGranular settings(std::vector<int16_t>& pcm) {
  InstrumentGranular g{};
  g.sample.data = pcm.data();
  g.sample.frameCount = (uint32_t)pcm.size();
  g.sample.channels = 1;
  g.sample.sampleRate = 48000;
  g.sample.sustain = 255;
  g.sample.envelopeShape = 128;
  g.grains = 16;
  g.sizeMs = 80;
  return g;
}
std::vector<float> render(const InstrumentGranular& g, int chunk = 256, int frames = 24000, float cents = 0) {
  GranularVoice voice;
  voice.init(48000);
  REQUIRE(voice.reserve(g.grains));
  voice.configure(g, cents, 1);
  voice.noteOn();
  std::vector<float> out((size_t)frames * 2);
  for (int offset = 0; offset < frames; offset += chunk)
    voice.render(out.data() + offset * 2, std::min(chunk, frames - offset));
  return out;
}
}

TEST_CASE("Granular count is not capped at a small performance limit and gain stays bounded") {
  std::vector<int16_t> pcm(4800, 16384);
  auto g = settings(pcm);
  for (uint16_t count : {1, 16, 256, 1024}) {
    g.grains = count;
    GranularVoice voice;
    voice.init(48000);
    REQUIRE(voice.reserve(count));
    voice.configure(g, 0, 1);
    CHECK(voice.grainCount() == count);
    std::vector<float> out(4096);
    voice.noteOn();
    voice.render(out.data(), 2048);
    CHECK(voice.active());
    for (float value : out) {
      REQUIRE(std::isfinite(value));
      CHECK(std::abs(value) <= 1.01f);
    }
    if (count >= 16) CHECK(out[4000] == doctest::Approx(0.5f).epsilon(0.01));
  }
}

TEST_CASE("Granular randomization and scanning do not depend on callback chunk size") {
  std::vector<int16_t> pcm(48000);
  for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = (int16_t)(24000 * std::sin(i * 0.07));
  auto g = settings(pcm);
  g.scatter = 160;
  g.pitchSpread = 700;
  g.width = 200;
  g.scanPercent = -125;
  const auto whole = render(g, 24000);
  const auto split = render(g, 127);
  REQUIRE(whole.size() == split.size());
  for (size_t i = 0; i < whole.size(); ++i)
    REQUIRE(whole[i] == doctest::Approx(split[i]).epsilon(0.002).scale(0.001));
}

TEST_CASE("Granular controls change the rendered audio and retriggers are deterministic") {
  std::vector<int16_t> pcm(48000);
  for (size_t i = 0; i < pcm.size(); ++i)
    pcm[i] = (int16_t)(22000 * std::sin(i * (i < 24000 ? 0.04 : 0.13)));
  auto g = settings(pcm);
  auto reference = render(g);
  CHECK(reference == render(g));
  CHECK(reference != render(g, 256, 24000, 1200));
  auto changed = g; changed.sample.start = 180; CHECK(reference != render(changed));
  changed = g; changed.sizeMs = 25; CHECK(reference != render(changed));
  changed = g; changed.scanPercent = 100; CHECK(reference != render(changed));
  changed = g; changed.scatter = 200; CHECK(reference != render(changed));
  changed = g; changed.pitchSpread = 1200; CHECK(reference != render(changed));
  changed = g; changed.width = 255; CHECK(reference != render(changed));
}

TEST_CASE("Granular empty input, growth and note off are safe") {
  GranularVoice voice;
  voice.init(48000);
  REQUIRE(voice.reserve(4));
  InstrumentGranular g{};
  g.grains = 4;
  voice.configure(g, 0, 1);
  voice.noteOn();
  CHECK_FALSE(voice.active());
  float out[512];
  std::fill(std::begin(out), std::end(out), 99);
  voice.render(out, 256);
  for (float v : out) CHECK(v == 0);
  std::vector<int16_t> pcm(4800, 16384);
  g = settings(pcm); g.grains = 4;
  voice.configure(g, 0, 1); voice.noteOn(); voice.render(out, 256);
  REQUIRE(voice.reserve(100));
  g.grains = 100;
  voice.configure(g, 0, 1);
  CHECK(voice.grainCount() == 100);
  voice.noteOff();
  for (int i = 0; i < 10; ++i) voice.render(out, 256);
  CHECK_FALSE(voice.active());
  voice.stopImmediately(); voice.render(out, 256);
  for (float v : out) CHECK(v == 0);
}
