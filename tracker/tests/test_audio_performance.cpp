#include "doctest.h"
#include "audio_performance.h"
#include "chipnomad_lib.h"
#include "app.h"
#include "screens.h"
#include "corelib_gfx.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace audioPerformance;
struct Enabled {
  Enabled() { setEnabled(true); reset(); }
  ~Enabled() { setEnabled(false); }
};
void window() {
  for (int i = 0; i < 10; ++i) {
    Callback callback(1200, 48000, now());
    if (sampling) {
      add(voices, 4000000);
      add(trackVoiceBase + 2, 4000000);
      add(instrumentBase + 7, 4000000);
    }
  }
}
}

TEST_CASE("Performance weights sampled work against sampled audio duration") {
  Enabled enabled;
  window();
  Snapshot result;
  REQUIRE(receive(result));
  CHECK(result.callbacks == 10);
  CHECK(result.sampled == 3);
  CHECK(result.average[voices] == 160);
  CHECK(result.average[trackVoiceBase + 2] == 160);
  CHECK(result.average[instrumentBase + 7] == 160);
  CHECK(result.average[instrumentBase + 6] == 0);
  CHECK(result.peak[voices] == 160);
  CHECK_FALSE(receive(result));
  {
    Callback callback(12000, 48000, now());
  }
  REQUIRE(receive(result));
  CHECK(result.average[voices] == 0);
  CHECK(result.peak[voices] == 160);
  reset();
  {
    Callback callback(12000, 48000, now());
  }
  REQUIRE(receive(result));
  CHECK(result.peak[voices] == 0);
}

TEST_CASE("Performance rejects old sessions and catches unsampled late callbacks") {
  Enabled enabled;
  window();
  reset();
  Snapshot result;
  CHECK_FALSE(receive(result));
  for (int i = 0; i < 10; ++i) {
    Callback callback(1200, 48000, now() - (i == 1 ? 30000000u : 0u));
    if (i == 1) CHECK_FALSE(sampling);
  }
  REQUIRE(receive(result));
  CHECK(result.overBudget >= 1);
  CHECK(result.peak[audioPerformance::callback] >= 1200);
  setEnabled(false);
  window();
  CHECK_FALSE(receive(result));
  CHECK_FALSE(sampling);
  setEnabled(true);
  window();
  REQUIRE(receive(result));
  CHECK(result.overBudget == 0);
}

TEST_CASE("Performance handoff never mixes snapshots across threads") {
  Enabled enabled;
  std::atomic<bool> done{false};
  std::thread producer([&] {
    for (int i = 0; i < 300; ++i) window();
    done.store(true, std::memory_order_release);
  });
  Snapshot result;
  int received = 0;
  do {
    if (receive(result)) {
      ++received;
      CHECK(result.average[voices] == 160);
      CHECK(result.average[trackVoiceBase + 2] == 160);
      CHECK(result.average[instrumentBase + 7] == 160);
      CHECK(result.callbacks == 10);
      CHECK(result.sampled >= 2);
      CHECK(result.sampled <= 3);
    }
  } while (!done.load(std::memory_order_acquire));
  producer.join();
  if (receive(result)) ++received;
  CHECK(received > 0);
}

TEST_CASE("Real engine rendering attributes voice paths and individual effects") {
  Enabled enabled;
  auto state = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  REQUIRE(state);
  auto& p = state->project;
  p.tracksCount = p.chipsCount = 1;
  getInstrumentFunctions(InstrumentType::Braids).init(&p.instruments[7]);
  p.trackReverbSend[0] = p.trackDelaySend[0] = 100;
  insertSelect(&p.trackInserts[0][0], insertDistortion);
  insertSelect(&p.masterInserts[0], insertDistortion);
  chipnomadInitChips(state.get(), 48000, nullptr);
  REQUIRE(chipnomadReserveRenderBuffers(state.get(), 12000) == 0);
  playbackPreviewNote(&state->playbackState, 0, 45, 7);
  std::vector<float> buffer(24000);
  for (int i = 0; i < 4; ++i) {
    Callback callback(12000, 48000, now());
    chipnomadRender(state.get(), buffer.data(), 12000);
  }
  Snapshot result;
  REQUIRE(receive(result));
  CHECK(result.peak[voices] > 0);
  CHECK(result.peak[trackVoiceBase] == result.peak[voices]);
  CHECK(result.peak[instrumentBase + 7] == result.peak[voices]);
  CHECK(result.peak[instrumentBase] == 0);
  CHECK(result.peak[trackVoiceBase + 1] == 0);
  CHECK(result.peak[trackInsertBase] > 0);
  CHECK(result.peak[masterInsert1] > 0);
  CHECK(result.peak[reverb] > 0);
  CHECK(result.peak[delay] > 0);
}

extern char mockGfxCells[20][40];
extern int mockCursorY;
extern int mockCursorX;

TEST_CASE("Performance tabs use logical directions only while the header has focus") {
  ChipNomadState* savedState = chipnomadState;
  const AppScreen* savedScreen = currentScreen;
  auto state = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  chipnomadState = state.get();
  currentScreen = &screenPerformance;
  screenPerformance.setup(0);
  screenPerformance.fullRedraw();
  CHECK(mockCursorY == 2);
  CHECK(mockCursorX == 0);
  REQUIRE(screenPerformance.onInput(1, keyRight, 0) == 1);
  CHECK(mockCursorX == 9);
  REQUIRE(screenPerformance.onInput(1, keyDown, 0) == 1);
  CHECK(mockCursorY == 3);
  REQUIRE(screenPerformance.onInput(1, keyDown, 0) == 1);
  CHECK(mockCursorY == 5);
  REQUIRE(screenPerformance.onInput(1, keyRight, 0) == 1);
  CHECK(std::string(mockGfxCells[6], 19) == "  Voice paths / mix");
  REQUIRE(screenPerformance.onInput(1, keyLeft, 0) == 1);
  CHECK(std::string(mockGfxCells[6], 9) == "+ Track 2");
  CHECK(screenPerformance.getPlaybackLevel() == ScreenPlaybackLevel::song);
  CHECK(screenPerformance.onInput(1, keyPlay, 0) == 0);
  REQUIRE(screenPerformance.onInput(1, keyOpt, 0) == 1);
  CHECK(currentScreen == &screenAudioSettings);
  Snapshot result;
  window();
  CHECK_FALSE(receive(result));
  chipnomadState = savedState;
  currentScreen = savedScreen;
}
