#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

#include "chipnomad_lib.h"
#include "audio_monitor.h"
#include "playback.h"
#include "project.h"
#include "project_utils.h"
#include "screens/screens.h"
#include "output_safety_limiter.h"
#include "synth/sample_voice.h"
#include "audio_input_manager.h"

namespace {
constexpr int kBlockFrames = 1024;
constexpr int kWarmupSeconds = 3;
#ifdef CCT_BENCHMARK_SECONDS
constexpr int kMeasuredSeconds = CCT_BENCHMARK_SECONDS;
#else
constexpr int kMeasuredSeconds = 20;
#endif
constexpr int kRounds = 5;

class LegacyOutputLimiter {
 public:
  void reset() {
    gain_ = 1.0f;
    previousInput_[0] = previousInput_[1] = 0.0f;
    previousOutput_[0] = previousOutput_[1] = 0.0f;
  }

  void process(const float* mix, int16_t* output, float* scratch, int frames, int sampleRate) {
    const int count = frames * 2;
    for (int i = 0; i < count; ++i) {
      const float sample = mix[i];
      output[i] = !std::isfinite(sample) ? 0 :
        (int16_t)(sample >= 1.0f ? 32767 : sample <= -1.0f ? -32768 : sample * 32767.0f);
    }
    const float dcDecay = std::exp(-94.24778f / sampleRate);
    double power = 0.0;
    float peak = 0.0f;
    for (int i = 0; i < count; ++i) {
      const float sample = output[i] / 32768.0f;
      const int channel = i & 1;
      const float filtered = sample - previousInput_[channel] + dcDecay * previousOutput_[channel];
      previousInput_[channel] = sample;
      previousOutput_[channel] = filtered;
      scratch[i] = filtered;
      power += filtered * filtered;
      peak = std::max(peak, std::fabs(filtered));
    }
    const float rms = std::sqrt(power / count);
    float target = 1.0f;
    if (peak > 0.8f) target = 0.8f / peak;
    if (rms > 0.35f) target = std::min(target, 0.35f / rms);
    gain_ = std::min(target, gain_ + frames / (0.25f * sampleRate));
    for (int i = 0; i < count; ++i)
      output[i] = static_cast<int16_t>(scratch[i] * gain_ * 32767.0f);
  }

 private:
  float gain_ = 1.0f;
  float previousInput_[2] = {};
  float previousOutput_[2] = {};
};

bool configureVariant(Project* project, const char* variant) {
  if (!std::strcmp(variant, "layer")) {
    int slot = PROJECT_MAX_INSTRUMENTS - 1;
    while (slot > 0 && project->instruments[slot].type != InstrumentType::none) --slot;
    if (slot == 0 || project->instruments[0].type == InstrumentType::none ||
        project->instruments[0].type == InstrumentType::Layer) {
      std::fprintf(stderr, "Layer benchmark needs instrument 00 and an empty instrument slot\n");
      return false;
    }
    Instrument& layer = project->instruments[slot];
    getInstrumentFunctions(InstrumentType::Layer).init(&layer);
    layer.volume = 255;
    layer.chip.layer.entries[0] = {0, 0x80, 255};
    for (auto& phrase : project->phrases)
      for (auto& row : phrase.rows)
        if (row.instrument == 0) row.instrument = (uint8_t)slot;
  } else if (!std::strcmp(variant, "no-effects")) {
    std::fill(project->trackReverbSend,
              project->trackReverbSend + PROJECT_MAX_TRACKS, 0);
    std::fill(project->trackDelaySend,
              project->trackDelaySend + PROJECT_MAX_TRACKS, 0);
    project->delayReverbSend = 0;
  } else if (!std::strcmp(variant, "no-tilt")) {
    std::fill(project->trackTilt,
              project->trackTilt + PROJECT_MAX_TRACKS, 0x80);
  } else if (!std::strcmp(variant, "no-drive")) {
    for (int i = 0; i < PROJECT_MAX_INSTRUMENTS; ++i) {
      Instrument& instrument = project->instruments[i];
      uint8_t* character = nullptr;
      if (instrument.type == InstrumentType::Braids)
        character = &instrument.chip.braids.filterCharacter;
      else if (instrument.type == InstrumentType::Sample)
        character = &instrument.chip.sample.filterCharacter;
      else if (instrument.type == InstrumentType::Plaits ||
               instrument.type == InstrumentType::PlaitsAlt)
        character = &instrument.chip.plaits.filterCharacter;
      else if (instrument.type == InstrumentType::SCWF)
        character = &instrument.chip.scwf.filterCharacter;
      else if (instrument.type == InstrumentType::BYOWTBL)
        character = &instrument.chip.byowtbl.filterCharacter;
      if (character && *character >= 3) *character = 1;
    }
  } else if (!std::strcmp(variant, "no-inserts")) {
    for (auto& inserts : project->trackInserts)
      for (auto& insert : inserts) insert.module = insertOff;
    for (auto& insert : project->masterInserts) insert.module = insertOff;
  }
  return true;
}

double render(Project* project, int sampleRate, int seconds, const char* variant,
              const char* pipeline, int startRow, volatile double* checksum,
              FILE* capture = nullptr) {
  ChipNomadState* state = chipnomadCreate();
  if (!state) return -1.0;
  state->ownsProjectResources = 0;
  state->project = *project;
  state->audioProject = *project;
  playbackInit(&state->playbackState, &state->audioProject);
  chipnomadInitChips(state, sampleRate, nullptr);
  chipnomadReserveRenderBuffers(state, kBlockFrames);
  state->benchmarkBypassPan = !std::strcmp(variant, "no-pan");
  if (!std::strcmp(variant, "no-monitor")) state->audioMonitor->setEnabled(false);
  playbackStartSong(&state->playbackState, startRow, 0, 1);

  std::vector<float> buffer(kBlockFrames * 2);
  std::vector<float> scratch(kBlockFrames * 2);
  std::vector<int16_t> output(kBlockFrames * 2);
  OutputSafetyLimiter limiter;
  LegacyOutputLimiter legacyLimiter;
  limiter.reset(sampleRate);
  legacyLimiter.reset();
  int remaining = seconds * sampleRate;
  const auto started = std::chrono::steady_clock::now();
  while (remaining > 0) {
    int frames = std::min(remaining, kBlockFrames);
    if (chipnomadRender(state, buffer.data(), frames) != frames) {
      chipnomadDestroy(state);
      return -1.0;
    }
    if (!std::strcmp(pipeline, "optimized"))
      limiter.process(buffer.data(), output.data(), frames);
    else if (!std::strcmp(pipeline, "legacy"))
      legacyLimiter.process(buffer.data(), output.data(), scratch.data(), frames, sampleRate);
    if (capture) {
      const bool rawFloat = !std::strcmp(pipeline, "none");
      const size_t count = (size_t)frames * 2;
      const size_t written = rawFloat
        ? std::fwrite(buffer.data(), sizeof(float), count, capture)
        : std::fwrite(output.data(), sizeof(int16_t), count, capture);
      if (written != count) {
        std::perror("Cannot write benchmark audio");
        chipnomadDestroy(state);
        return -1.0;
      }
    }
    const int sample = (remaining / kBlockFrames) % (frames * 2);
    *checksum += !std::strcmp(pipeline, "none") ? buffer[sample] : output[sample];
    remaining -= frames;
  }
  const double elapsed = std::chrono::duration<double>(
    std::chrono::steady_clock::now() - started).count();
  chipnomadDestroy(state);
  return elapsed;
}
int benchmarkSamplerStretch(const char* path) {
  constexpr int sampleRate = 32000, frames = 2048, blocks = 320;
  struct Mode { SampleAlgorithm id; const char* label; };
  constexpr Mode modes[] = {
    {SampleAlgorithm::Repitch, "Repitch"}, {SampleAlgorithm::Granular, "Granular"},
    {SampleAlgorithm::Wsola, "WSOLA"}, {SampleAlgorithm::Beat, "Beat"},
    {SampleAlgorithm::BeatPro, "Beat Pro"}, {SampleAlgorithm::Spectral, "Spectral"}
  };
  for (int tracks : {4, 8}) for (const auto& mode : modes) {
    ChipNomadState* state = chipnomadCreate();
    if (!state) return 1;
    Project& project = state->project;
    projectInitAY(&project);
    project.tracksCount = tracks;
    project.tickRate = 48;
    auto& instrument = project.instruments[0];
    getInstrumentFunctions(InstrumentType::Sample).init(&instrument);
    auto& sample = instrument.chip.sample;
    char error[96];
    if (sampleLoadWav16(path, &sample, error, sizeof(error))) {
      std::fprintf(stderr, "%s: %s\n", path, error);
      chipnomadDestroy(state);
      return 1;
    }
    sample.timing.algorithm = (uint8_t)mode.id;
    samplePrepareBeatOnsets(&sample);
    sample.stretchMode = 5;
    sample.loopMode = 1;
    sample.filterEnabled = 0;
    sample.sustain = 255;
    for (int track = 0; track < tracks; ++track) {
      project.song[0][track] = 0;
      project.trackVolume[track] = 80;
    }
    project.chains[0].rows[0].phrase = 0;
    for (int row = 1; row < 4; ++row) project.chains[0].rows[row].phrase = 1;
    phraseClear(&project.phrases[0]);
    phraseClear(&project.phrases[1]);
    auto& note = project.phrases[0].rows[0];
    note.instrument = 0;
    note.note = 48;
    note.volume = 80;
    chipnomadInitChips(state, sampleRate, nullptr);
    if (chipnomadReserveRenderBuffers(state, frames) ||
        !chipnomadQueuePlaybackStartSong(state, 0, 0, 1)) {
      chipnomadDestroy(state);
      return 1;
    }
    std::vector<double> times;
    times.reserve(blocks);
    float out[2 * frames];
    int overBudget = 0;
    double peak = 0;
    const double budget = 1000.0 * frames / sampleRate;
    for (int block = 0; block < blocks; ++block) {
      const auto start = std::chrono::steady_clock::now();
      chipnomadRender(state, out, frames);
      const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
      overBudget += elapsed > budget;
      times.push_back(elapsed);
      for (float value : out) if (std::isfinite(value)) peak = std::max(peak, (double)std::abs(value));
    }
    chipnomadDestroy(state);
    if (peak == 0) {
      std::fprintf(stderr, "%s: benchmark rendered silence\n", mode.label);
      return 1;
    }
    std::sort(times.begin(), times.end());
    std::printf("%d tracks %-8s p95 %6.2f ms, max %6.2f ms, > %.0f ms: %d/%d\n",
                tracks, mode.label, times[blocks * 95 / 100 - 1], times.back(), budget, overBudget, blocks);
  }
  std::puts("Offline engine render at 32 kHz / 2048 frames / Stretch 4bar; no device underrun measurement.");
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && !std::strcmp(argv[1], "--sampler-stretch"))
    return benchmarkSamplerStretch(argc > 2 ? argv[2] : "packaging/common/SR_wavetables/WaveEdit/AMEN.WAV");
  if (argc > 1 && !std::strcmp(argv[1], "--audio-ring")) {
    AudioInputRing ring;
    int16_t input[kBlockFrames * 2], output[kBlockFrames * 2];
    for (int i = 0; i < kBlockFrames * 2; ++i) input[i] = (int16_t)(i * 31);
    constexpr int rounds = 20000;
    volatile int checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; ++i) {
      if (!ring.push(input, kBlockFrames) || ring.pop(output, kBlockFrames) != kBlockFrames) return 1;
      checksum += output[i & (kBlockFrames * 2 - 1)];
    }
    const double elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - started).count();
    std::printf("audio ring: %.6f s, %.2f ns/frame, checksum %d\n", elapsed,
      elapsed * 1.0e9 / (rounds * kBlockFrames), (int)checksum);
    return 0;
  }
  const char* path = argc > 1 ? argv[1] : "packaging/common/projects/alf dance.cct";
  const int sampleRate = argc > 2 ? std::atoi(argv[2]) : 96000;
  const char* variant = argc > 3 ? argv[3] : "full";
  const char* pipeline = argc > 4 ? argv[4] : "optimized";
  const int startRow = argc > 5 ? (int)std::strtol(argv[5], nullptr, 0) : 0;
  if (sampleRate != 32000 && sampleRate != 44100 && sampleRate != 48000 && sampleRate != 96000) {
    std::fprintf(stderr, "Sample rate must be 32000, 44100, 48000 or 96000\n");
    return 1;
  }
  Project project;
  projectInit(&project);
  if (projectLoad(&project, path)) {
    std::fprintf(stderr, "Cannot load %s: %s\n", path, projectFileError);
    return 1;
  }
  const char* variants[] = {"full", "layer", "no-effects", "no-tilt", "no-drive", "no-inserts", "no-pan", "no-monitor"};
  bool knownVariant = false;
  for (const char* candidate : variants) knownVariant |= !std::strcmp(candidate, variant);
  if (!knownVariant) {
    std::fprintf(stderr, "Unknown variant: %s\n", variant);
    return 1;
  }
  if (std::strcmp(pipeline, "optimized") && std::strcmp(pipeline, "legacy") &&
      std::strcmp(pipeline, "none")) {
    std::fprintf(stderr, "Output pipeline must be optimized, legacy or none\n");
    return 1;
  }
  if (!configureVariant(&project, variant)) {
    projectFree(&project);
    return 1;
  }
  std::printf("variant: %s, output: %s, sample rate: %d, start row: %02X\n",
              variant, pipeline, sampleRate, startRow);

  volatile double checksum = 0.0;
  if (render(&project, sampleRate, kWarmupSeconds, variant, pipeline, startRow, &checksum) < 0.0)
    return 1;
  std::vector<double> results;
  for (int round = 0; round < kRounds; ++round) {
    double elapsed = render(&project, sampleRate, kMeasuredSeconds, variant, pipeline, startRow, &checksum);
    if (elapsed < 0.0) return 1;
    results.push_back(elapsed);
    std::printf("round %d: %.6f s (%.2f%% realtime)\n", round + 1, elapsed,
                elapsed * 100.0 / kMeasuredSeconds);
  }
  std::sort(results.begin(), results.end());
  double median = results[results.size() / 2];
  std::printf("median: %.6f s, %.2f%% realtime, %.2f ns/frame, checksum %.9f\n",
              median, median * 100.0 / kMeasuredSeconds,
              median * 1.0e9 / (kMeasuredSeconds * sampleRate),
              static_cast<double>(checksum));
  // Optional final argument: capture a separate pass, outside timed rounds.
  if (argc > 6) {
    FILE* capture = std::fopen(argv[6], "wb");
    if (!capture) {
      std::perror("Cannot open benchmark audio");
      projectFree(&project);
      return 1;
    }
    const double captured = render(&project, sampleRate, kMeasuredSeconds, variant,
                                   pipeline, startRow, &checksum, capture);
    const int closeResult = std::fclose(capture);
    if (closeResult) std::perror("Cannot close benchmark audio");
    if (captured < 0.0 || closeResult) {
      projectFree(&project);
      return 1;
    }
    std::printf("Captured %d seconds of stereo %s audio to %s\n", kMeasuredSeconds,
                !std::strcmp(pipeline, "none") ? "float32" : "int16", argv[6]);
  }
  projectFree(&project);
  return 0;
}
