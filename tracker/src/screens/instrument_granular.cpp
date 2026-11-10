#include "screen_instrument.h"
#include "audio_manager.h"
#include "field_style.h"
#include "file_browser.h"
#include "corelib_gfx.h"
#include "synth/sample_voice.h"
#include "utils.h"
#include <cstdio>
#include <cstring>

namespace {
InstrumentGranular& current() { return chipnomadState->project.instruments[cInstrument].chip.granular; }
int columns(int row) { return row < 3 ? instrumentCommonColumnCount(row) : row == 3 ? 1 : row == 9 ? 5 : 2; }
void cancelled() {
  audioManager.stopSamplePreview();
  screenSetup(&screenInstrument, cInstrument);
}
void preview(const char* path) {
  if (audioManager.previewSample(path)) screenMessage(MESSAGE_TIME_ERROR, "Cannot preview WAV");
}
void loaded(const char* path) {
  audioManager.stopSamplePreview();
  char error[64];
  audioManager.pause();
  int result = sampleLoadWav16(path, &current().sample, error, sizeof(error));
  if (!result) {
    current().sample.start = 0;
    chipnomadInitChips(chipnomadState, appSettings.audioSampleRate, nullptr);
    chipnomadQueueProjectRefresh(chipnomadState);
    projectModified = 1;
  }
  audioManager.resume();
  if (result) screenMessage(MESSAGE_TIME_ERROR, "%s", error);
  screenSetup(&screenInstrument, cInstrument);
}
void drawStatic() {
  instrumentCommonDrawStatic();
  gfxSetFgColor(appSettings.colorScheme.textTitles);
  gfxPrint(0, 6, "Source");
  gfxPrint(0, 14, "ADSR");
  if (chipnomadState->granularAllocationFailed)
    screenMessage(MESSAGE_TIME_ERROR, "Not enough grain memory");
}
void cursor(int col, int row) {
  if (row < 3) return instrumentCommonDrawCursor(col, row);
  if (row == 9) { instrumentCommonDrawVoicePostCursor(col, row); return; }
  gfxCursor(row == 3 ? 8 : kColumnValueX[col], row == 3 ? 6 : row + 4, row == 3 ? 24 : 7);
}
void field(int col, int row, CellState state) {
  if (row < 3) return instrumentCommonDrawField(col, row, state);
  auto& g = current();
  if (row == 9) { instrumentCommonDrawVoicePostField(col, row, state, &g.sample); return; }
  if (row == 3) {
    const char* name = std::strrchr(g.sample.path, '/');
    name = name ? name + 1 : g.sample.path;
    gfxSetFgColor(fieldNameColor(state));
    gfxClearRect(8, 6, 24, 1);
    gfxPrintf(8, 6, "%.24s", name[0] ? name : "Load WAV");
    return;
  }
  static const char* labels[5][2] = {{"Pos", "Pitch"}, {"Size", "Grains"},
    {"Scan", "Scatter"}, {"PitRnd", "Width"}, {"Cutoff", "Reso"}};
  const int x = kColumnValueX[col], y = row + 4;
  fieldDrawLabel(kColumnNameX[col], y, labels[row - 4][col], state);
  gfxClearRect(x, y, 7, 1);
  gfxSetFgColor(fieldValueColor(state));
  if (row == 4 && !col) gfxPrint(x, y, byteToHex(g.sample.start));
  else if (row == 4) gfxPrintf(x, y, "%+d st", g.sample.pitch);
  else if (row == 5 && !col) gfxPrintf(x, y, "%u ms", g.sizeMs);
  else if (row == 5) gfxPrintf(x, y, "%u", g.grains);
  else if (row == 6 && !col) gfxPrintf(x, y, "%+d%%", g.scanPercent);
  else if (row == 6) gfxPrint(x, y, byteToHex(g.scatter));
  else if (row == 7 && !col) gfxPrintf(x, y, "%u ct", g.pitchSpread);
  else if (row == 7) gfxPrint(x, y, byteToHex(g.width));
  else if (row == 8 && !col) gfxPrintf(x, y, "%u", g.sample.filterCutoffHz);
  else gfxPrint(x, y, byteToHex(g.sample.filterResonance));
}
int edit(int col, int row, CellEditAction action) {
  if (row < 3) return instrumentCommonOnEdit(col, row, action);
  auto& g = current();
  int handled = 0;
  if (row == 3) {
    if (action != CellEditAction::tap) return 0;
    fileBrowserSetupWithPreview("LOAD GRAIN SOURCE", ".wav", appSettings.samplePath,
      loaded, cancelled, preview, 0);
    screenSetup(&screenFileBrowser, 0);
    return 1;
  }
  if (row == 9) handled = instrumentCommonOnEditVoicePost(col, row, action, &g.sample);
  else if (row == 4 && !col) handled = edit8noLast(action, &g.sample.start, 16, 0, 255);
  else if (row == 4) handled = editSigned8(action, &g.sample.pitch, 12, -48, 48);
  else if (row == 5 && !col) handled = edit16withMinMax(action, &g.sizeMs, 20, 1, 2000);
  else if (row == 5) {
    uint16_t count = g.grains;
    handled = edit16withMinMax(action, &count, 16, 1, 65535);
    if (handled && count != g.grains) {
      audioManager.pause();
      int failed = chipnomadReserveGranularGrains(chipnomadState, count);
      if (!failed) g.grains = count;
      audioManager.resume();
      if (failed) screenMessage(MESSAGE_TIME_ERROR, "Not enough grain memory");
    }
  }
  else if (row == 6 && !col) handled = editSigned16(action, &g.scanPercent, 25, -400, 400);
  else if (row == 6) handled = edit8noLast(action, &g.scatter, 16, 0, 255);
  else if (row == 7 && !col) handled = edit16withMinMax(action, &g.pitchSpread, 100, 0, 2400);
  else if (row == 7) handled = edit8noLast(action, &g.width, 16, 0, 255);
  else if (row == 8 && !col) handled = editFilterCutoff(action, &g.sample.filterCutoffHz);
  else if (row == 8) handled = edit8noLast(action, &g.sample.filterResonance, 16, 0, 255);
  if (handled) projectModified = 1;
  return handled;
}
}
ScreenData screenInstrumentGranular = {
  10, 0, 0, 0, -1, 0, 0, 0, 0, ScreenPlaybackLevel::none, columns, drawStatic,
  cursor, nullptr, nullptr, nullptr, field, edit, nullptr, nullptr, nullptr, nullptr
};
