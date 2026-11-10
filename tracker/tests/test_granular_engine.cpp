#include "doctest.h"
#include "synth/granular_voice.h"
#include "chipnomad_lib.h"
#include "project.h"
#include "app.h"
#include "screens/screen_instrument.h"
#include "mocks/app_ui_mock.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

TEST_CASE("Granular instruments round trip with embedded WAV data") {
  auto source = std::make_unique<Project>();
  auto restored = std::make_unique<Project>();
  projectInit(source.get()); projectInit(restored.get());
  auto& instrument = source->instruments[0];
  getInstrumentFunctions(InstrumentType::Granular).init(&instrument);
  auto& g = instrument.chip.granular;
  g.grains = 513; g.sizeMs = 173; g.scanPercent = -75;
  g.scatter = 191; g.pitchSpread = 731; g.width = 219;
  g.sample.frameCount = 480; g.sample.sampleRate = 48000; g.sample.channels = 1;
  g.sample.data = (int16_t*)std::malloc(480 * sizeof(int16_t));
  REQUIRE(g.sample.data);
  for (int i = 0; i < 480; ++i) g.sample.data[i] = i * 50;
  const char* path = "test-granular-roundtrip.cct";
  REQUIRE(projectSave(source.get(), path) == 0);
  REQUIRE(projectLoad(restored.get(), path) == 0);
  const auto& result = restored->instruments[0];
  CHECK(result.type == InstrumentType::Granular);
  CHECK(result.chip.granular.grains == 513);
  CHECK(result.chip.granular.sizeMs == 173);
  CHECK(result.chip.granular.scanPercent == -75);
  CHECK(result.chip.granular.scatter == 191);
  CHECK(result.chip.granular.pitchSpread == 731);
  CHECK(result.chip.granular.width == 219);
  REQUIRE(result.chip.granular.sample.data);
  CHECK(result.chip.granular.sample.frameCount == 480);
  CHECK(result.chip.granular.sample.data[300] == 15000);
  projectFree(source.get()); projectFree(restored.get());
  std::remove(path);
}

TEST_CASE("Granular engine voices receive notes, FX and release through the real render path") {
  auto state = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  REQUIRE(state);
  auto& instrument = state->project.instruments[0];
  getInstrumentFunctions(InstrumentType::Granular).init(&instrument);
  auto& g = instrument.chip.granular;
  g.sample.frameCount = 4800; g.sample.sampleRate = 48000; g.sample.channels = 1;
  g.sample.data = (int16_t*)std::malloc(4800 * sizeof(int16_t));
  REQUIRE(g.sample.data);
  std::fill(g.sample.data, g.sample.data + 4800, 16000);
  chipnomadInitChips(state.get(), 48000, nullptr);
  REQUIRE(chipnomadReserveRenderBuffers(state.get(), 512) == 0);
  REQUIRE(state->granularVoices[0][0]);
  CHECK(state->granularVoices[0][0]->capacity() >= 16);
  REQUIRE(chipnomadReserveGranularGrains(state.get(), 257) == 0);
  g.grains = 257;
  chipnomadQueueProjectRefresh(state.get());
  playbackPreviewNote(&state->playbackState, 0, 48, 0);
  float out[1024]{};
  double peak = 0;
  for (int b = 0; b < 8; ++b) {
    chipnomadRender(state.get(), out, 512);
    for (float v : out) peak = std::max(peak, std::abs((double)v));
  }
  CHECK(peak > 0.01);
  CHECK(state->granularVoices[0][0]->grainCount() == 257);
  chipnomadQueuePlaybackStop(state.get());
  for (int b = 0; b < 8; ++b) chipnomadRender(state.get(), out, 512);
  for (float v : out) CHECK(v == 0);
}

TEST_CASE("Granular instrument page opens the WAV browser and grows memory before editing the count") {
  struct Restore {
    ChipNomadState* state = chipnomadState;
    int instrument = cInstrument, modified = projectModified;
    const AppScreen* screen = currentScreen;
    ~Restore() {
      chipnomadState = state; cInstrument = instrument;
      currentScreen = screen; projectModified = modified;
    }
  } restore;
  auto state = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  REQUIRE(state);
  chipnomadState = state.get();
  cInstrument = 0;
  currentScreen = &screenInstrument;
  getInstrumentFunctions(InstrumentType::Granular).init(&state->project.instruments[0]);
  chipnomadInitChips(state.get(), 48000, nullptr);
  auto& page = screenInstrumentGranular;
  page.drawField(1, 5, CellState::focus);
  CHECK(std::string(mockGfxCells[9] + 25, 2) == "16");
  REQUIRE(page.onEdit(1, 5, CellEditAction::increase) == 1);
  CHECK(state->project.instruments[0].chip.granular.grains == 17);
  CHECK(state->granularVoices[0][0]->capacity() >= 17);
  REQUIRE(page.onEdit(0, 6, CellEditAction::decrease) == 1);
  CHECK(state->project.instruments[0].chip.granular.scanPercent == -1);
  REQUIRE(page.onEdit(0, 3, CellEditAction::tap) == 1);
  CHECK(std::string(mockBrowserTitle) == "LOAD GRAIN SOURCE");
  CHECK(std::string(mockBrowserExtension) == ".wav");
  CHECK(currentScreen == &screenFileBrowser);
}
