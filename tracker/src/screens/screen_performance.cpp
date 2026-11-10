#include "screens.h"
#include "app.h"
#include "audio_performance.h"
#include "corelib_gfx.h"
#include "insert_fx.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace {
using namespace audioPerformance;
struct Row {
  char label[24];
  int key, track;
};
Row rows[PROJECT_MAX_INSTRUMENTS + 48];
int rowCount, tab, cursor = -2, top;
bool expanded[PROJECT_MAX_TRACKS]{};
Snapshot snapshot;
uint64_t nextRefresh, nextMemory;
long residentKiB = -1;
const char* tabs[] = {"Overview", "Tracks", "Instruments", "FX"};
const int tabX[] = {0, 9, 16, 28};

uint64_t uiNow() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}
void append(const char* label, int key, int track = -1) {
  auto& row = rows[rowCount++];
  std::snprintf(row.label, sizeof(row.label), "%s", label);
  row.key = key;
  row.track = track;
}
void buildRows() {
  rowCount = 0;
  char label[40];
  if (tab == 0) {
    append("Callback", callback);
    append("Voice paths", voices);
    append("Sequencer", sequencer);
    append("Track EQ", trackEQ);
    append("Track inserts", inserts);
    append("Delay", delay);
    append("Reverb", reverb);
    append("Master insert 1", masterInsert1);
    append("Master insert 2", masterInsert2);
    append("Master EQ", masterEQ);
    append("Output / limiter", output);
    append("Other / overhead", -2);
  } else if (tab == 1) {
    for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) {
      std::snprintf(label, sizeof(label), "%c Track %d", expanded[t] ? '-' : '+', t + 1);
      append(label, -1, t);
      if (!expanded[t]) continue;
      append("  Voice paths / mix", trackVoiceBase + t);
      append("  EQ", trackEQBase + t);
      for (int slot = 0; slot < 2; ++slot) {
        std::snprintf(label, sizeof(label), "  %d %.17s", slot + 1,
          insertDescriptor(chipnomadState->project.trackInserts[t][slot].module).name);
        append(label, trackInsertBase + t * 2 + slot);
      }
    }
  } else if (tab == 2) {
    for (int i = 0; i < PROJECT_MAX_INSTRUMENTS; ++i) {
      const auto& instrument = chipnomadState->project.instruments[i];
      if (instrument.type == InstrumentType::none && !snapshot.peak[instrumentBase + i]) continue;
      std::snprintf(label, sizeof(label), "%02X %.19s", i, instrument.name);
      append(label, instrumentBase + i);
    }
  } else {
    append("Delay", delay);
    append("Reverb", reverb);
    append("Master EQ", masterEQ);
    for (int slot = 0; slot < 2; ++slot) {
      std::snprintf(label, sizeof(label), "M%d %.19s", slot + 1,
        insertDescriptor(chipnomadState->project.masterInserts[slot].module).name);
      append(label, masterInsert1 + slot);
    }
    for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) {
      std::snprintf(label, sizeof(label), "T%d EQ", t + 1);
      append(label, trackEQBase + t);
      for (int slot = 0; slot < 2; ++slot) {
        std::snprintf(label, sizeof(label), "T%d.%d %.17s", t + 1, slot + 1,
          insertDescriptor(chipnomadState->project.trackInserts[t][slot].module).name);
        append(label, trackInsertBase + t * 2 + slot);
      }
    }
  }
  cursor = std::min(cursor, rowCount - 1);
  top = std::max(0, std::min(top, std::max(0, rowCount - 12)));
  if (cursor >= 0) {
    if (cursor < top) top = cursor;
    if (cursor >= top + 12) top = cursor - 11;
  }
}
uint32_t average(const Row& row) {
  if (row.key >= 0) return snapshot.average[row.key];
  if (row.track >= 0) {
    int t = row.track;
    return snapshot.average[trackVoiceBase + t] + snapshot.average[trackEQBase + t] +
      snapshot.average[trackInsertBase + t * 2] + snapshot.average[trackInsertBase + t * 2 + 1];
  }
  uint32_t accounted = 0;
  for (int key = voices; key <= output; ++key) accounted += snapshot.average[key];
  return snapshot.average[callback] > accounted ? snapshot.average[callback] - accounted : 0;
}
void readMemory() {
#ifdef __linux__
  residentKiB = -1;
  if (FILE* file = std::fopen("/proc/self/status", "r")) {
    char line[160];
    while (std::fgets(line, sizeof(line), file))
      if (std::sscanf(line, "VmRSS: %ld kB", &residentKiB) == 1) break;
    std::fclose(file);
  }
#endif
}
void paint() {
  buildRows();
  const auto cs = appSettings.colorScheme;
  gfxClearRect(0, 0, 33, 19);
  gfxSetFgColor(cs.textTitles);
  gfxPrint(0, 0, "PERFORMANCE");
  for (int i = 0; i < 4; ++i) {
    gfxSetFgColor(i == tab ? cs.textValue : cs.textEmpty);
    gfxPrint(tabX[i], 2, tabs[i]);
  }
  gfxSetFgColor(cs.textLabel);
  gfxPrint(0, 3, "Reset peaks");
  gfxPrint(23, 4, "AVG% PEAK%");
  for (int i = top; i < std::min(rowCount, top + 12); ++i) {
    int y = 5 + i - top;
    const auto& row = rows[i];
    gfxSetFgColor(i == cursor ? cs.textValue : cs.textDefault);
    gfxPrintf(0, y, "%.22s", row.label);
    uint32_t avg = average(row);
    if (avg < 10000) gfxPrintf(23, y, "%3u.%u", avg / 10, avg % 10);
    else gfxPrint(23, y, ">999");
    if (row.key < 0) gfxPrint(29, y, "   -");
    else {
      uint32_t peak = snapshot.peak[row.key];
      if (peak < 10000) gfxPrintf(28, y, "%3u.%u", peak / 10, peak % 10);
      else gfxPrint(28, y, ">999");
    }
  }
  if (!rowCount) {
    gfxSetFgColor(cs.textEmpty);
    gfxPrint(0, 5, "No instruments");
  }
  gfxSetFgColor(cs.textInfo);
  if (tab == 0) {
    if (residentKiB >= 0) gfxPrintf(0, 17, "RSS %ld.%ld MiB  Late %u", residentKiB / 1024,
      residentKiB % 1024 * 10 / 1024, snapshot.overBudget);
    else gfxPrintf(0, 17, "RSS n/a  Late %u", snapshot.overBudget);
  } else gfxPrintf(0, 17, "Rows %d-%d / %d", rowCount ? top + 1 : 0, std::min(rowCount, top + 12), rowCount);
  gfxPrint(0, 18, "Detail 1/4; 100%=buffer budget");
  if (cursor == -2) gfxCursor(tabX[tab], 2, (int)std::strlen(tabs[tab]));
  else if (cursor == -1) gfxCursor(0, 3, 11);
  else gfxCursor(0, 5 + cursor - top, 22);
}
void setup(int) {
  tab = 0;
  cursor = -2;
  top = 0;
  snapshot = Snapshot{};
  std::memset(expanded, 0, sizeof(expanded));
  nextRefresh = nextMemory = 0;
  setEnabled(true);
  reset();
}
void draw() {
  uint64_t time = uiNow();
  if (time < nextRefresh) return;
  nextRefresh = time + 250;
  receive(snapshot);
  if (time >= nextMemory) {
    readMemory();
    nextMemory = time + 1000;
  }
  paint();
}
int input(int down, int keys, int taps) {
  if (!down) return 0;
  if (keys == keyOpt) {
    setEnabled(false);
    screenSetup(&screenAudioSettings, 0);
    return 1;
  }
  if (keys == keyUp) cursor = std::max(-2, cursor - 1);
  else if (keys == keyDown) cursor = std::min(rowCount - 1, cursor + 1);
  else if (cursor == -2 && (keys == keyLeft || keys == keyRight)) {
    tab = (tab + (keys == keyRight ? 1 : 3)) % 4;
    top = 0;
  } else if (keys == keyEdit && taps == 1 && cursor == -1) {
    reset();
    snapshot = Snapshot{};
  } else if (cursor >= 0 && tab == 1 && rows[cursor].track >= 0 &&
      ((keys == keyEdit && taps == 1) || keys == keyRight || keys == keyLeft)) {
    int track = rows[cursor].track;
    expanded[track] = keys == keyRight ? true : keys == keyLeft ? false : !expanded[track];
  } else return 0;
  paint();
  return 1;
}
ScreenPlaybackLevel playbackLevel() { return ScreenPlaybackLevel::song; }
}
const AppScreen screenPerformance = {nullptr, setup, paint, draw, input, playbackLevel};
