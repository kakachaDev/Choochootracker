#include "sample_voice.h"
#include "sample_timing.h"

#include "../project_instruments.h"
#include "../project_io_common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <array>

namespace {
constexpr size_t kGrainWindowSegments = 2048;
// Shared by all voices and output rates; initialized before audio rendering.
const std::array<float, kGrainWindowSegments + 1> grainWindow = [] {
  std::array<float, kGrainWindowSegments + 1> table{};
  for (size_t i = 0; i <= kGrainWindowSegments; ++i) {
    const float window = sinf(3.14159265f * ((float)i / kGrainWindowSegments));
    table[i] = window * window;
  }
  return table;
}();

float grainWindowAt(uint32_t age, float scale) {
  const float position = age * scale;
  const size_t index = (size_t)position;
  if (index >= kGrainWindowSegments) return grainWindow.back();
  return grainWindow[index] + (grainWindow[index + 1] - grainWindow[index]) * (position - index);
}
} // namespace

#ifdef CCT_DISABLE_CONFIG_CACHE
static constexpr bool kCacheConfigurations = false;
#else
static constexpr bool kCacheConfigurations = true;
#endif

static void sampleStorePath(const char* path, InstrumentSample* sample) {
  char workingDirectory[1024];
  const char* storedPath = path;
  if (getcwd(workingDirectory, sizeof(workingDirectory))) {
    size_t length = strlen(workingDirectory);
#ifdef _WIN32
    if (strlen(path) > length && _strnicmp(path, workingDirectory, length) == 0 &&
#else
    if (strlen(path) > length && strncmp(path, workingDirectory, length) == 0 &&
#endif
        (path[length] == '/' || path[length] == '\\')) {
      storedPath = path + length + 1;
    }
  }
  strncpy(sample->path, storedPath, sizeof(sample->path) - 1);
  sample->path[sizeof(sample->path) - 1] = 0;
}

static float envelopeTime(uint8_t value) {
  float normalized = value / 255.0f;
  return normalized * normalized * 5.0f;
}

void SampleVoice::init(float outputSampleRate) {
  sample_ = nullptr;
  outputSampleRate_ = outputSampleRate;
  position_ = 0.0;
  step_ = 1.0;
  stepConfigured_ = false;
  direction_ = 1;
  reverse_ = false;
  loopMode_ = 0;
  timeStretch_ = 1.0f;
  granular_ = false;
  grainExhausted_ = false;
  startFrame_ = 0;
  endFrame_ = 0;
  active_ = false;
  lastOutput_[0] = lastOutput_[1] = 0.0f;
  rampFrom_[0] = rampFrom_[1] = 0.0f;
  rampRemaining_ = 0;
  rampFrames_ = outputSampleRate > 500.0f ? (uint32_t)(outputSampleRate * 0.002f) : 1;
  tailFrom_[0] = tailFrom_[1] = 0.0f;
  transitionFrames_ = 0;
  transitionPending_ = false;
  grainSize_ = (uint32_t)(outputSampleRate_ * 0.040f);
  grainHop_ = grainSize_ / 2;
  grainWindowScale_ = kGrainWindowSegments / (float)(grainSize_ > 1 ? grainSize_ - 1 : 1);
  // 3 ms de-click tail, at least 2 frames so the linear weight never
  // divides by zero.
  transitionLength_ = outputSampleRate > 500.0f ? (uint32_t)(outputSampleRate * 0.003f) : 2;
  if (transitionLength_ < 2) transitionLength_ = 2;
  useStretch_ = false;
  algorithm_ = SampleAlgorithm::Legacy;
  timingPaused_ = false;
  pauseRemaining_ = 0;
  timingGrains_ = false;
#if defined(PORTMASTER_BUILD) || defined(ANDROID_BUILD) || defined(WEB_BUILD)
  stretch_.init(outputSampleRate_, true);
#else
  stretch_.init(outputSampleRate_, false);
#endif
  post_.init(outputSampleRate_);
}

uint8_t sampleNormalizeSlice(uint8_t slice) {
  // Accept any count 1..64: EQUAL/AUTO use power-of-2 counts, but LAZY
  // slices are hand-placed and may use any count.
  if (slice >= 1 && slice <= 64) return slice;
  return 0;
}

uint8_t sampleEncodeSlice(SliceMode mode, uint8_t count) {
  if (mode == sliceModeOff) return 0;
  if (count < 1) count = 1;
  if (count > 64) count = 64;
  switch (mode) {
    case sliceModeEqual: return (uint8_t)(0 + count);
    case sliceModeAuto: return (uint8_t)(64 + count);
    case sliceModeLazy: return (uint8_t)(128 + count);
    default: return 0;
  }
}

SliceMode sampleDecodeSliceMode(uint8_t slice) {
  if (slice == 0) return sliceModeOff;
  if (slice <= 64) return sliceModeEqual;
  if (slice <= 128) return sliceModeAuto;
  if (slice <= 192) return sliceModeLazy;
  return sliceModeOff; // 193..255 reserved
}

uint8_t sampleDecodeSliceCount(uint8_t slice) {
  switch (sampleDecodeSliceMode(slice)) {
    case sliceModeEqual: return slice;
    case sliceModeAuto: return (uint8_t)(slice - 64);
    case sliceModeLazy: return (uint8_t)(slice - 128);
    default: return 0;
  }
}

uint8_t sampleNormalizeSliceEx(uint8_t slice) {
  if (slice <= 192) return slice;
  return 0;
}

int sampleActsAsSliced(const InstrumentSample* sample) {
  if (!sample || sample->stretchMode) return 0;
  const SliceMode mode = sampleDecodeSliceMode(sample->slice);
  return mode != sliceModeOff;
}

// --- Slice bounds editing (Phase 1, universal editing model) -------------

uint8_t sampleSliceBoundCount(const InstrumentSample* sample) {
  if (!sample) return 0;
  if (sampleDecodeSliceMode(sample->slice) == sliceModeOff) return 0;
  return sampleDecodeSliceCount(sample->slice);
}

// Loop region in frames shared by the bounds helpers: swaps inverted
// markers and falls back to the whole sample for an empty region (same
// rules as SampleVoice::configure).
static void sampleSliceLoopRegion(const InstrumentSample* sample, uint8_t start, uint8_t end,
                                  uint32_t* loopStart, uint32_t* loopEnd) {
  uint32_t s = sampleMarkerToStartFrame(sample->frameCount, start);
  uint32_t e = sampleMarkerToEndFrame(sample->frameCount, end);
  if (s > e) {
    uint32_t swap = s;
    s = e;
    e = swap + 1;
  }
  if (e <= s) e = sample->frameCount;
  *loopStart = s;
  *loopEnd = e;
}

uint8_t sampleSliceInitEven(InstrumentSample* sample, SliceMode mode, uint8_t count) {
  if (!sample) return 0;
  if (count < 1) count = 1;
  if (count > PROJECT_SAMPLE_MAX_SLICES) count = PROJECT_SAMPLE_MAX_SLICES;
  uint32_t loopStart = 0;
  uint32_t loopEnd = sample->frameCount;
  if (sample->frameCount > 0) {
    sampleSliceLoopRegion(sample, sample->start, sample->end, &loopStart, &loopEnd);
  }
  const uint32_t loopLength = loopEnd > loopStart ? loopEnd - loopStart : 0;
  memset(sample->sliceBounds, 0, sizeof(sample->sliceBounds));
  for (uint8_t i = 0; i < count; ++i) {
    sample->sliceBounds[i] = loopStart + (uint32_t)((uint64_t)loopLength * i / count);
  }
  sample->slice = sampleEncodeSlice(mode, count);
  return count;
}

uint8_t sampleSliceInitLazy(InstrumentSample* sample) {
  if (!sample) return 0;
  memset(sample->sliceBounds, 0, sizeof(sample->sliceBounds));
  sample->slice = sampleEncodeSlice(sliceModeLazy, 1);
  return 1;
}

int sampleSliceSplit(InstrumentSample* sample, uint8_t index) {
  if (!sample) return -1;
  const uint8_t count = sampleSliceBoundCount(sample);
  if (count == 0 || index >= count) return -1;
  if (count >= PROJECT_SAMPLE_MAX_SLICES) return -1;
  // The last slice ends at the loop end marker; use it as the right edge
  // when splitting the final slice.
  uint32_t loopEnd = sample->frameCount;
  if (sample->frameCount > 0) {
    uint32_t loopStart = 0;
    sampleSliceLoopRegion(sample, sample->start, sample->end, &loopStart, &loopEnd);
  }
  const uint32_t left = sample->sliceBounds[index];
  const uint32_t right = index + 1 < count ? sample->sliceBounds[index + 1] : loopEnd;
  if (right <= left + 1) return -1; // nothing to split
  const uint32_t mid = left + (right - left) / 2;
  for (int i = count; i > (int)index + 1; --i) {
    sample->sliceBounds[i] = sample->sliceBounds[i - 1];
  }
  sample->sliceBounds[index + 1] = mid;
  sample->slice = sampleEncodeSlice(sampleDecodeSliceMode(sample->slice), (uint8_t)(count + 1));
  return index + 1;
}

int sampleSliceDelete(InstrumentSample* sample, uint8_t index) {
  if (!sample) return -1;
  const uint8_t count = sampleSliceBoundCount(sample);
  if (count == 0 || index >= count) return -1;
  if (count == 1) {
    // Deleting the last slice turns slicing off; keep the bounds in memory
    // so toggling back to a mode can restore them (plan §4.2 task 2).
    sample->slice = 0;
    return -1;
  }
  for (uint8_t i = index; i < count - 1; ++i) {
    sample->sliceBounds[i] = sample->sliceBounds[i + 1];
  }
  sample->sliceBounds[count - 1] = 0;
  sample->slice = sampleEncodeSlice(sampleDecodeSliceMode(sample->slice), (uint8_t)(count - 1));
  return index > 0 ? index - 1 : 0;
}

int sampleSliceInsertAtFrame(InstrumentSample* sample, uint32_t frame) {
  if (!sample) return -1;
  const uint8_t count = sampleSliceBoundCount(sample);
  if (count == 0 || count >= PROJECT_SAMPLE_MAX_SLICES) return -1;
  uint32_t loopStart = 0;
  uint32_t loopEnd = sample->frameCount;
  if (sample->frameCount > 0) {
    sampleSliceLoopRegion(sample, sample->start, sample->end, &loopStart, &loopEnd);
  }
  if (frame < loopStart || frame >= loopEnd) return -1;
  // Find the insertion point (bounds are kept sorted ascending) and reject
  // frames that duplicate an existing bound.
  uint8_t insertAt = count;
  for (uint8_t i = 0; i < count; ++i) {
    if (sample->sliceBounds[i] == frame) return -1;
    if (sample->sliceBounds[i] > frame) {
      insertAt = i;
      break;
    }
  }
  for (int i = count; i > (int)insertAt; --i) {
    sample->sliceBounds[i] = sample->sliceBounds[i - 1];
  }
  sample->sliceBounds[insertAt] = frame;
  sample->slice = sampleEncodeSlice(sampleDecodeSliceMode(sample->slice), (uint8_t)(count + 1));
  return insertAt;
}

int sampleSliceInsertAtFrameGapped(InstrumentSample* sample, uint32_t frame,
                                   uint32_t minGapFrames) {
  if (!sample) return -1;
  const uint8_t count = sampleSliceBoundCount(sample);
  if (count == 0 || count >= PROJECT_SAMPLE_MAX_SLICES) return -1;
  uint32_t loopStart = 0;
  uint32_t loopEnd = sample->frameCount;
  if (sample->frameCount > 0) {
    sampleSliceLoopRegion(sample, sample->start, sample->end, &loopStart, &loopEnd);
  }
  if (frame < loopStart || frame >= loopEnd) return -1;
  // Reject frames too close to an existing bound (or to the loop start, so
  // the first slice never collapses to nothing).
  if (frame - loopStart < minGapFrames) return -1;
  for (uint8_t i = 0; i < count; ++i) {
    const uint32_t bound = sample->sliceBounds[i];
    if (frame > bound ? frame - bound < minGapFrames : bound - frame < minGapFrames) {
      return -1;
    }
  }
  return sampleSliceInsertAtFrame(sample, frame);
}

int32_t sampleSliceNudge(InstrumentSample* sample, uint8_t index, int32_t delta) {
  if (!sample) return -1;
  const uint8_t count = sampleSliceBoundCount(sample);
  if (count == 0 || index >= count) return -1;
  uint32_t loopEnd = sample->frameCount;
  if (sample->frameCount > 0) {
    uint32_t loopStart = 0;
    sampleSliceLoopRegion(sample, sample->start, sample->end, &loopStart, &loopEnd);
  }
  const uint32_t left = index > 0 ? sample->sliceBounds[index - 1] : 0;
  const uint32_t right = index + 1 < count ? sample->sliceBounds[index + 1] : loopEnd;
  int64_t next = (int64_t)sample->sliceBounds[index] + delta;
  if (next < (int64_t)left) next = left;
  if (next > (int64_t)right) next = right;
  if (next == (int64_t)sample->sliceBounds[index]) return -1;
  sample->sliceBounds[index] = (uint32_t)next;
  return (int32_t)next;
}

int32_t sampleSliceStartFrame(const InstrumentSample* sample, uint8_t index,
                              uint8_t start, uint8_t end) {
  if (!sample) return -1;
  const uint8_t count = sampleSliceBoundCount(sample);
  if (count == 0 || index >= count) return -1;
  // Populated bounds win; legacy samples (all bounds zero) fall back to the
  // even division of the loop region.
  if (sample->sliceBounds[0] != 0 || (count > 1 && sample->sliceBounds[1] != 0)) {
    return (int32_t)sample->sliceBounds[index];
  }
  uint32_t loopStart = 0;
  uint32_t loopEnd = sample->frameCount;
  if (sample->frameCount > 0) {
    sampleSliceLoopRegion(sample, start, end, &loopStart, &loopEnd);
  }
  const uint32_t loopLength = loopEnd > loopStart ? loopEnd - loopStart : 0;
  return (int32_t)(loopStart + (uint32_t)((uint64_t)loopLength * index / count));
}

void sampleSliceFrames(uint32_t frameCount, uint8_t sliceCount, uint8_t sliceIndex,
                       uint32_t* startFrame, uint32_t* endFrame) {
  if (!startFrame || !endFrame) return;
  if (frameCount == 0 || sliceCount == 0) {
    *startFrame = 0;
    *endFrame = frameCount;
    return;
  }
  if (sliceIndex >= sliceCount) sliceIndex = sliceCount - 1;
  *startFrame = (uint32_t)((uint64_t)sliceIndex * frameCount / sliceCount);
  *endFrame = (uint32_t)((uint64_t)(sliceIndex + 1) * frameCount / sliceCount);
  if (*endFrame <= *startFrame) *endFrame = *startFrame + 1;
  if (*endFrame > frameCount) *endFrame = frameCount;
}

void SampleVoice::configure(const InstrumentSample* sample, float pitchCents,
                            float gain, float speedPercent, uint8_t start, uint8_t end, uint8_t loopMode,
                            uint16_t cutoffHz, uint8_t resonance, int attack, int decay,
                            int sustain, int release, int envelopeShape, uint8_t sliceCount,
                            uint8_t sliceIndex, uint8_t stretchMode, float tickRateHz,
                            uint8_t speedAlgorithm, uint8_t forceReverse, uint8_t loopStart,
                            uint8_t xfade, const SampleTimeSettings* timing) {
  sample_ = sample;
  timing_ = timing;
  post_.setGain(gain);
  if (!sample_ || !sample_->data || sample_->frameCount == 0) return;

  const auto algorithm = timing_ ? sampleNormalizeAlgorithm(timing_->algorithm) : SampleAlgorithm::Legacy;
  const bool changedAlgorithm = algorithm_ != algorithm;
  const bool wasTimingPaused = timingPaused_;
  const auto previousStart = startFrame_;
  const auto previousEnd = endFrame_;
  const bool previousReverse = reverse_;
  const double previousStep = step_;
  const uint8_t previousLoopMode = loopMode_;
  const uint32_t previousLoopStart = active_ ? loopStartFrame_ : 0;
  algorithm_ = algorithm;
  const bool useSliceMarks = stretchMode != 0 && sample_->slice &&
    (algorithm == SampleAlgorithm::Beat || algorithm == SampleAlgorithm::BeatPro);
  if (beatUsesSlices_ != useSliceMarks) beatDirection_ = 0;
  beatUsesSlices_ = useSliceMarks;
  const bool legacy = algorithm == SampleAlgorithm::Legacy;
  useStretch_ = legacy ? sliceCount == 0 &&
    (stretchMode != 0 || (speedAlgorithm == 1 && speedPercent != 0))
    : algorithm == SampleAlgorithm::Spectral;
  if (legacy && useStretch_) {
    // The stretcher derives reverse playback from the marker order; swap
    // the markers when SPL 01 forces reverse on an ascending window.
    uint8_t stretchStart = start;
    uint8_t stretchEnd = end;
    if (forceReverse && start <= end) {
      stretchStart = end;
      stretchEnd = start;
    }
    stretch_.configure(sample, stretchMode, speedPercent, tickRateHz, pitchCents / 100.0f, stretchStart, stretchEnd, loopMode, loopStart);
  }

  uint32_t startFrame;
  uint32_t endFrame;
  // Phase 1: sliceCount arrives already decoded from the sentinel by the
  // caller. Samples with populated sliceBounds play their stored boundaries
  // (manual edits, AUTO detection); legacy samples with empty bounds keep
  // the even-division fallback.
  sliceCount = sampleNormalizeSlice(sliceCount);
  if (sliceCount) {
    // When slicing is enabled, divide the LOOP REGION (start to end) into slices
    // This ensures slices follow the start/end markers
    uint32_t loopStartFrame = sampleMarkerToStartFrame(sample_->frameCount, start);
    uint32_t loopEndFrame = sampleMarkerToEndFrame(sample_->frameCount, end);
    if (loopStartFrame > loopEndFrame) {
      uint32_t swap = loopStartFrame;
      loopStartFrame = loopEndFrame;
      loopEndFrame = swap + 1;
    }
    uint32_t loopLength = loopEndFrame > loopStartFrame ? loopEndFrame - loopStartFrame : sample_->frameCount;

    const int boundsPopulated = sample_->sliceBounds[0] != 0 ||
      (sliceCount > 1 && sample_->sliceBounds[1] != 0);
    if (boundsPopulated && sliceIndex < sliceCount) {
      // Stored boundaries: this slice starts at its bound and ends at the
      // next bound (the loop end for the last slice).
      startFrame_ = sample_->sliceBounds[sliceIndex];
      endFrame_ = sliceIndex + 1 < sliceCount ? sample_->sliceBounds[sliceIndex + 1]
                                              : loopEndFrame;
      reverse_ = forceReverse != 0;
    } else {
      uint32_t sliceStart, sliceEnd;
      sampleSliceFrames(loopLength, sliceCount, sliceIndex, &sliceStart, &sliceEnd);

      // Map slice to absolute frame positions within the loop region
      startFrame_ = loopStartFrame + sliceStart;
      endFrame_ = loopStartFrame + sliceEnd;
      reverse_ = forceReverse != 0;
    }
  } else {
    startFrame = sampleMarkerToStartFrame(sample_->frameCount, start);
    endFrame = sampleMarkerToEndFrame(sample_->frameCount, end);
    // SPL 01 forces reverse regardless of the marker order; the legacy
    // Start > End marker convention still applies when not forced. The
    // window is stored ascending either way - reverse playback walks it
    // from the high end down (see noteOn).
    reverse_ = forceReverse != 0 || start > end;
    if (startFrame > endFrame) {
      uint32_t swap = startFrame;
      startFrame = endFrame;
      endFrame = swap;
    }
    startFrame_ = startFrame;
    endFrame_ = endFrame;
  }
  if (endFrame_ <= startFrame_) endFrame_ = startFrame_ + 1;
  if (endFrame_ > sample_->frameCount) endFrame_ = sample_->frameCount;
  if (algorithm != SampleAlgorithm::Repitch &&
      (!kCacheConfigurations || !stepConfigured_ || stepSampleRate_ != sample_->sampleRate ||
       stepPitchCents_ != pitchCents)) {
    const float sourceRate = sample_->sampleRate ? sample_->sampleRate : outputSampleRate_;
    pitchStep_ = (sourceRate / outputSampleRate_) * pow(2.0, pitchCents / 1200.0);
    stepSampleRate_ = sample_->sampleRate;
    stepPitchCents_ = pitchCents;
    stepConfigured_ = true;
  }
  step_ = pitchStep_;
  timeStretch_ = speedPercent / 100.0f;
  granular_ = legacy && fabsf(timeStretch_ - 1.0f) > 0.001f;
  const bool wasTimingGrains = timingGrains_;
  timingGrains_ = algorithm == SampleAlgorithm::Granular || algorithm == SampleAlgorithm::Wsola;
  timingPaused_ = false;
  if (!legacy) {
    // User slices still use Speed. Stretch and user slicing remain mutually exclusive.
    if (sliceCount) stretchMode = 0;
    const double sourceRate = sample_->sampleRate ? sample_->sampleRate : outputSampleRate_;
    const double length = endFrame_ - startFrame_;
    const double seconds = sampleTargetSeconds(length / sourceRate, stretchMode, tickRateHz, speedPercent);
    timingPaused_ = seconds <= 0;
    step_ = timingPaused_ ? 0.0 : length / (seconds * outputSampleRate_);
    if (useStretch_ && !timingPaused_) {
      const auto stretchLoop = xfade && (loopMode == 1 || loopMode == 3) ? loopMode + 1 : loopMode;
      stretch_.configureWindow(sample, stretchMode, speedPercent, tickRateHz, pitchCents / 100.0f,
                               startFrame_, endFrame_, reverse_, stretchLoop, loopStart);
    }
    // A direct PCM read is identical at unity rate and pitch.
    if (timingGrains_ && std::abs(step_ - pitchStep_) < 1e-6) timingGrains_ = false;
  }
  if (!kCacheConfigurations) {
    grainSize_ = (uint32_t)(outputSampleRate_ * 0.040f);
    grainHop_ = grainSize_ / 2;
  }
  if (timingPaused_ && !wasTimingPaused) {
    pauseFrom_[0] = lastOutput_[0];
    pauseFrom_[1] = lastOutput_[1];
    pauseRemaining_ = rampFrames_;
  }
  // Instrument loop modes: 0 off, 1 loop, 2 loop with X-fade, 3 ping-pong,
  // 4 ping-pong with X-fade. The X-fade flag comes from the mode itself;
  // the explicit xfade parameter stays for the SPL lane FX path, which can
  // turn the crossfade on for modes 1/3.
  loopMode_ = loopMode > 4 ? 0 : loopMode;
  bool xfadeOn = xfade != 0 || loopMode_ == 2 || loopMode_ == 4;
  // Loop wrap point: the LOOP START marker when a loop mode is active, clamped
  // inside the playback region (Region START <= LOOP START < Region END). With
  // no loop the marker is inert and startFrame_ keeps the old behavior
  // bit-identical. Region edits that move the markers re-clamp loopStart in
  // the sample editor. Placed after the loopMode_ assignment so the wrap
  // point follows the loop mode being configured, not the previous one.
  if (loopMode_ != 0) {
    loopStartFrame_ = sampleMarkerToStartFrame(sample_->frameCount, loopStart);
    if (loopStartFrame_ < startFrame_) loopStartFrame_ = startFrame_;
    if (loopStartFrame_ >= endFrame_) loopStartFrame_ = endFrame_ - 1;
  } else {
    loopStartFrame_ = startFrame_;
  }
  // X-fade window: 25% of the loop window (the wrap span), in source
  // frames. SPL 04/05 turn the crossfade on; without them the engine stays
  // disabled and the plain path is bit-identical to the pre-X-fade build.
  // A window shorter than one frame cannot fade - the hard wrap stays.
  // configure() runs every tick, so only reconfigure when the window
  // actually changes: an unconditional configure() would reset an in-flight
  // fade at the tick boundary.
  uint32_t fadeLen = 0;
  if (xfadeOn && loopMode_ != 0) {
    const uint32_t loopLength = endFrame_ - loopStartFrame_;
    fadeLen = loopLength / 4;
    // A fade shorter than one output frame cannot be sampled safely.
    if (fadeLen <= step_) fadeLen = 0;
  }
  if (!legacy && xfade_.active() && (fadeLen != xfade_.length() || previousStep != step_ ||
      previousLoopStart != loopStartFrame_ || previousLoopMode != loopMode_)) {
    position_ = xfade_.outgoingPosition();
    xfade_.reset();
    if (position_ < startFrame_ || position_ >= endFrame_) position_ = startFrame_;
  }
  if (fadeLen != xfade_.length()) xfade_.configure(fadeLen);
  post_.setFilter(sample_->filterEnabled != 0, sample_->filterCharacter, sample_->filterMode,
                  sample_->filterSlope24dB != 0, cutoffHz, resonance / 255.0f, sample_->filterColor);
  post_.setEnvelope(true, envelopeTime(attack < 0 ? sample_->attack : attack),
                    envelopeTime(decay < 0 ? sample_->decay : decay),
                    (sustain < 0 ? sample_->sustain : sustain) / 255.0f,
                    envelopeTime(release < 0 ? sample_->release : release),
                    envelopeShape < 0 ? sample_->envelopeShape : envelopeShape);
  if (active_) {
    if (changedAlgorithm) {
      stretch_.reset();
      noteOn();
    } else if (!legacy && (previousStart != startFrame_ || previousEnd != endFrame_ || previousReverse != reverse_)) {
      // Start/End can be modulated at audio rate. Keep the envelope and
      // in-range cursor; only rebuild state that depends on the old region.
      if (previousReverse != reverse_) direction_ = reverse_ ? -1 : 1;
      if (position_ < startFrame_ || position_ >= endFrame_) {
        position_ = direction_ < 0 ? endFrame_ - 1.0 : startFrame_;
        rampFrom_[0] = lastOutput_[0];
        rampFrom_[1] = lastOutput_[1];
        rampRemaining_ = rampFrames_;
        beatDirection_ = 0;
      }
      if (beatDirection_) {
        uint32_t first, end;
        beatBounds(first, end);
        if (previousReverse != reverse_ || first != beatFirst_ || end != beatEnd_) beatDirection_ = 0;
      }
      xfade_.reset();
      if (timingGrains_) resetTimingGrains(position_, direction_ * pitchStep_, grainHop_);
    } else if (!timingPaused_ && (wasTimingPaused || (timingGrains_ && !wasTimingGrains))) {
      resetTimingGrains(position_, direction_ * pitchStep_, grainHop_);
      if (wasTimingPaused) {
        rampFrom_[0] = rampFrom_[1] = 0.0f;
        rampRemaining_ = rampFrames_;
      }
    }
  }
}

void SampleVoice::noteOn() {
  if (!sample_ || !sample_->data || endFrame_ <= startFrame_) return;
  for (int channel = 0; channel < 2; ++channel)
    rampFrom_[channel] =
        (active_ || transitionFrames_ || transitionPending_) ? lastOutput_[channel] : 0.0f;
  rampRemaining_ = rampFrames_;
  // A new note must not resume a fade armed for the previous one: the
  // output ramp above already bridges from the last rendered output.
  transitionPending_ = false;
  transitionFrames_ = 0;
  post_.kill(); // Restart sample attack; the output ramp bridges the previous waveform.
  beatDirection_ = 0;
  timingLooped_ = false;
  if (useStretch_) {
    // A new note always restarts the stretched playback from the top of the
    // source window: tracks are monophonic, so a retrigger must kill the
    // previous playhead and every note keeps its full stretch length.
    // (Stretch enabled mid-note WITHOUT a retrigger lazy-primes in render().)
    stretch_.noteOn();
    active_ = true;
    post_.noteOn(true);
    return;
  }
  // A new note must not resume a fade armed for the previous one.
  xfade_.reset();
  position_ = reverse_ ? (algorithm_ == SampleAlgorithm::Legacy ? endFrame_ - 1.0 :
                          std::nextafter((double)endFrame_, (double)startFrame_)) : startFrame_;
  direction_ = reverse_ ? -1 : 1;
  grainExhausted_ = false;
  if (timingGrains_) resetTimingGrains(position_, direction_ * pitchStep_, grainHop_);
  grainPosition_[0] = reverse_ ? endFrame_ - 1 : startFrame_;
  grainPosition_[1] = grainPosition_[0] + direction_ * step_ * grainHop_ * timeStretch_;
  grainAge_[0] = grainHop_;
  grainAge_[1] = 0;
  nextGrainPosition_ = grainPosition_[1] + direction_ * step_ * grainHop_ * timeStretch_;
  active_ = true;
  post_.noteOn(true);
}

float SampleVoice::sampleAt(double position, int channel) const {
  uint32_t frame = (uint32_t)position;
  // Direction-independent interpolation: the fractional part always
  // interpolates toward frame + 1. Mirroring the fraction toward frame - 1
  // for descending reads zigzags around every integer frame and buzzes on
  // low-rate samples (step < 1); the stretch processor reads this way for
  // the same reason.
  uint32_t next = frame + 1 < endFrame_ ? frame + 1 : frame;
  int sourceChannel = sample_->channels == 1 ? 0 : channel;
  float a = sample_->data[frame * sample_->channels + sourceChannel] / 32768.0f;
  float b = sample_->data[next * sample_->channels + sourceChannel] / 32768.0f;
  return a + (b - a) * (float)(position - frame);
}

float SampleVoice::grainSampleAt(double position, int channel) const {
  // With a loop mode active the granular fold uses the LOOP START marker as
  // the lower bound; without a loop it keeps the region start.
  const double first = loopMode_ != 0 ? loopStartFrame_ : startFrame_;
  const double length = endFrame_ - first;
  const double passStart = timingLooped_ ? first : startFrame_;
  if (loopMode_ == 1 || loopMode_ == 2) {
    if (position >= passStart && position < endFrame_) {
      // Inside the region: no fold, the first pass plays through.
    } else {
      position = fmod(position - first, length);
      if (position < 0.0) position += length;
      position += first;
    }
  } else if (loopMode_ == 3 || loopMode_ == 4) {
    if (position >= passStart && position < endFrame_) {
      // Inside the region: no fold, the first pass plays through.
    } else {
      if (length <= 1.0) {
        position = first;
      } else {
        const double span = length - 1.0;
        const double period = span * 2.0;
        position = fmod(position - first, period);
        if (position < 0.0) position += period;
        position = first + (position <= span ? position : period - position);
      }
    }
  } else if (position < first || position >= endFrame_) {
    return 0.0f;
  }
  // Same direction-independent interpolation as sampleAt: mirroring the
  // fraction for descending grains buzzes on low-rate material.
  uint32_t frame = (uint32_t)position;
  uint32_t next = frame + 1 < endFrame_ ? frame + 1 : frame;
  int sourceChannel = sample_->channels == 1 ? 0 : channel;
  float a = sample_->data[frame * sample_->channels + sourceChannel] / 32768.0f;
  float b = sample_->data[next * sample_->channels + sourceChannel] / 32768.0f;
  return a + (b - a) * (float)(position - frame);
}

void SampleVoice::resetTimingGrains(double origin, double readStep, uint32_t hop) {
  timingHead_[0] = origin - hop * readStep;
  timingAge_[0] = hop;
  timingAge_[1] = 2 * hop;
  timingReadStep_[0] = timingReadStep_[1] = readStep;
  timingHead_[1] = origin;
}

template <typename Read>
void SampleVoice::timingGrainFrame(float* output, double nominal, double readStep, uint32_t hop,
                                   bool align, const Read& read) {
  const int channels = sample_->channels == 1 ? 1 : 2;
  for (int g = 0; g < 2; ++g) {
    if (timingAge_[g] < 2 * hop) continue;
    double offset = 0;
    if (align) {
      const int other = 1 - g;
      const double reference = timingHead_[other] + timingAge_[other] * timingReadStep_[other];
      constexpr int probes = 12;
      float target[probes][2];
      float referenceEnergy = 0;
      for (int n = 0; n < probes; ++n) for (int ch = 0; ch < channels; ++ch) {
        const float v = read(reference + (n * hop / probes) * timingReadStep_[other], ch);
        target[n][ch] = v;
        referenceEnergy += v * v;
      }
      // Nine candidates at most, one stereo-coherent shift per grain.
      // Prefer the nominal hop on ties or silent overlaps.
      float best = -2;
      if (referenceEnergy > 1e-7f) for (int i : {0, -1, 1, -2, 2, -3, 3, -4, 4}) {
        const double shift = (i * hop / 20.0) * readStep;
        float dot = 0, energy = 0;
        for (int n = 0; n < probes; ++n) for (int ch = 0; ch < channels; ++ch) {
          const float v = read(nominal + shift + (n * hop / probes) * readStep, ch);
          dot += target[n][ch] * v;
          energy += v * v;
        }
        const float score = dot / sqrtf(referenceEnergy * energy + 1e-12f) - 0.02f * abs(i);
        if (score > best) { best = score; offset = shift; }
      }
    }
    timingHead_[g] = nominal + offset;
    timingReadStep_[g] = readStep;
    timingAge_[g] = 0;
  }
  const float scale = (float)kGrainWindowSegments / (2 * hop);
  const float a = grainWindowAt(timingAge_[0], scale);
  const float b = grainWindowAt(timingAge_[1], scale);
  for (int ch = 0; ch < channels; ++ch) {
    output[ch] = a * read(timingHead_[0] + timingAge_[0] * timingReadStep_[0], ch)
              + b * read(timingHead_[1] + timingAge_[1] * timingReadStep_[1], ch);
  }
  if (channels == 1) output[1] = output[0];
  ++timingAge_[0];
  ++timingAge_[1];
}

void SampleVoice::beatBounds(uint32_t& start, uint32_t& end) const {
  const uint32_t frame = (uint32_t)position_;
  const uint32_t* first = timing_->beatOnsets;
  const uint32_t* last = first + std::min<unsigned>(timing_->beatOnsetCount, PROJECT_SAMPLE_MAX_SLICES);
  const uint32_t* next = std::upper_bound(first, last, frame);
  start = next == first ? startFrame_ : std::max(startFrame_, next[-1]);
  if (timingLooped_ || (direction_ < 0 && loopMode_ >= 3))
    start = std::max(start, loopStartFrame_);
  end = next == last ? endFrame_ : std::min(endFrame_, *next);
  if (beatUsesSlices_) {
    start = timingLooped_ || (direction_ < 0 && loopMode_ >= 3) ? loopStartFrame_ : startFrame_;
    end = endFrame_;
    const uint8_t count = sampleSliceBoundCount(sample_);
    const bool stored = count && (sample_->sliceBounds[0] || (count > 1 && sample_->sliceBounds[1]));
    for (uint8_t i = 0; i < count; ++i) {
      const uint32_t mark = stored ? sample_->sliceBounds[i]
        : startFrame_ + (uint32_t)((uint64_t)(endFrame_ - startFrame_) * i / count);
      if (mark <= frame) start = std::max(start, mark);
      else end = std::min(end, mark);
    }
  }
}

void SampleVoice::prepareBeatTail() {
  beatTailStart_ = beatTailEnd_ = 0;
  if (algorithm_ != SampleAlgorithm::BeatPro) return;
  // Bound the analysis cost per hit regardless of PCM length. Use stereo
  // peak envelopes across bins so individual zero crossings are immaterial.
  constexpr unsigned bins = 48, probes = 24;
  const uint32_t length = beatEnd_ - beatFirst_;
  const double body = (sample_->sampleRate ? sample_->sampleRate : outputSampleRate_) * 0.045;
  if (length <= body || length < bins) return;
  int peaks[bins]{};
  int peak = 0;
  for (unsigned i = 0; i < bins; ++i) {
    const uint32_t first = (uint64_t)length * i / bins;
    const uint32_t end = (uint64_t)length * (i + 1) / bins;
    const unsigned n = std::min<unsigned>(probes, end - first);
    for (unsigned p = 0; p < n; ++p) {
      const uint32_t offset = first + (uint64_t)(end - first) * p / n;
      const uint32_t frame = direction_ > 0 ? beatFirst_ + offset : beatEnd_ - 1 - offset;
      const int16_t* pcm = sample_->data + (size_t)frame * sample_->channels;
      peaks[i] = std::max(peaks[i], sample_->channels == 1 ? abs(pcm[0])
                                                      : std::max(abs(pcm[0]), abs(pcm[1])));
    }
    peak = std::max(peak, peaks[i]);
  }
  const int quiet = std::max(24, peak / 70);
  unsigned last = bins;
  while (last && peaks[last - 1] <= quiet) --last;
  // Keep intentional gaps, including isolated 808 hits, at their natural decay.
  if (last != bins) return;
  unsigned first = last;
  while (first && peaks[first - 1] <= peak / 3) --first;
  if (last - first < 3) return;
  const int beginning = peaks[first];
  if (beginning <= quiet || peaks[last - 1] * 4 > beginning * 3) return;
  beatTailStart_ = std::max(body, (double)length * first / bins);
  beatTailEnd_ = (double)length * last / bins;
  if (beatTailEnd_ - beatTailStart_ < pitchStep_ * 64) beatTailEnd_ = 0;
}

void SampleVoice::beatFrame(float* output) {
  if (beatDirection_ != direction_ || position_ < beatFirst_ || position_ >= beatEnd_) {
    beatBounds(beatFirst_, beatEnd_);
    beatDirection_ = direction_;
    beatElapsed_ = 0;
    beatWarmup_ = 0;
    prepareBeatTail();
  }
  const double sourceLength = beatEnd_ - beatFirst_;
  const double outward = direction_ > 0 ? beatEnd_ - position_ : position_ - beatFirst_ + 1;
  const double origin = direction_ > 0 ? beatFirst_ : beatEnd_ - 1;
  const double signedPitch = direction_ * pitchStep_;
  const double attack = beatTailStart_;
  const double natural = beatElapsed_ * pitchStep_;
  const double remaining = outward / step_;
  auto read = [&](double pos, int ch) {
    if (pos < beatFirst_ || pos >= beatEnd_) return 0.0f;
    // Never interpolate into the following event.
    return sampleAt(std::min(pos, beatEnd_ - 1.0), ch);
  };
  const double tail = beatTailEnd_ - attack;
  const bool stretchingTail = natural >= attack && tail > 0 && step_ < pitchStep_;
  if (stretchingTail) {
    const double tailElapsed = beatElapsed_ - attack / pitchStep_;
    const double total = tailElapsed + remaining;
    const double nominal = origin + direction_ * (attack + tail * tailElapsed / total);
    if (!beatWarmup_) {
      beatHop_ = std::max(1u, (uint32_t)std::min(tail / pitchStep_ / 3, (double)grainHop_));
      resetTimingGrains(origin + direction_ * natural, signedPitch, beatHop_);
      beatWarmup_ = 1;
    }
    timingGrainFrame(output, nominal, signedPitch, beatHop_, true, [&](double pos, int ch) {
      const double offset = direction_ * (pos - origin);
      if (offset >= beatTailEnd_) return 0.0f;
      return read(offset < attack ? origin + direction_ * attack : pos, ch);
    });
  } else {
    beatWarmup_ = 0;
    const double pos = origin + direction_ * natural;
    output[0] = read(pos, 0);
    output[1] = sample_->channels == 1 ? output[0] : read(pos, 1);
  }
  // Limit the old tail before the next attack and fade to silence when Beat
  // exhausts a segment early. No fades are applied to the incoming attack.
  const double naturalRemaining = (sourceLength - natural) / pitchStep_;
  const double fadeRemaining = stretchingTail ? remaining : std::min(remaining, naturalRemaining);
  // When the tail is still audible, ease it out rather than hard-cut it.
  const double fadeSeconds = stretchingTail ? 0.012 : 0.002;
  const float gain = (float)std::clamp(fadeRemaining / std::max(1.0, outputSampleRate_ * fadeSeconds), 0.0, 1.0);
  output[0] *= gain;
  output[1] *= gain;
  ++beatElapsed_;
}

double SampleVoice::seamDistance() const {
  // Source-frame distance from the playhead to the next seam, in the
  // direction of travel. The seam is where advancePosition would fold:
  // forward passes wrap at the region end, descending passes at the region
  // start, and a descending ping-pong pass bounces at the loop start
  // marker. The distance is in SOURCE frames (not step-scaled) so the fade
  // window covers the same stretch of material regardless of playback
  // speed.
  if ((loopMode_ == 3 || loopMode_ == 4) && direction_ < 0) {
    return position_ - loopStartFrame_;
  }
  if (direction_ < 0) return position_ - startFrame_;
  return endFrame_ - position_;
}

void SampleVoice::foldTimingPosition(double& position, int& direction) const {
  const double first = loopStartFrame_;
  const bool ping = loopMode_ >= 3;
  const double span = endFrame_ - first - (ping ? 1 : 0);
  if (span <= 0) {
    position = first;
  } else if (ping) {
    // Unfold onto a forward-only timeline; handles rates larger than the loop span.
    const double period = span * 2;
    double phase = fmod(direction > 0 ? position - first : period - (position - first), period);
    if (phase < 0) phase += period;
    direction = phase < span ? 1 : -1;
    position = first + (phase <= span ? phase : period - phase);
  } else {
    double phase = fmod(position - first, span);
    if (phase < 0) phase += span;
    position = first + phase;
  }
}

void SampleVoice::wrap() {
  // The loop wraps between the LOOP START marker and the region end.
  double first = loopStartFrame_;
  double last = endFrame_ - 1.0;
  if (algorithm_ != SampleAlgorithm::Legacy) {
    foldTimingPosition(position_, direction_);
    beatDirection_ = 0;
    timingLooped_ = true;
  } else if (loopMode_ == 1 || loopMode_ == 2) {
    double length = endFrame_ - loopStartFrame_;
    while (position_ >= endFrame_) position_ -= length;
    while (position_ < loopStartFrame_) position_ += length;
  } else if (last <= first) {
    position_ = first;
  } else if (position_ > last) {
    position_ = last - (position_ - last);
    direction_ = -1;
  } else {
    position_ = first + (first - position_);
    direction_ = 1;
  }
}

void SampleVoice::finishLoopFade() {
  if (algorithm_ != SampleAlgorithm::Legacy) {
    // Blend in the next pass without charging the fade's lookahead to the
    // musical loop period. Advance and fold the outgoing clock as usual.
    position_ = xfade_.outgoingPosition();
  } else {
    xfade_.takeIncoming(&position_, &direction_);
  }
  if (position_ < loopStartFrame_ || position_ >= endFrame_) wrap();
}

bool SampleVoice::advancePosition() {
  position_ += step_ * direction_;
  // A descending ping-pong pass bounces at the loop start marker; every
  // other path keeps the region bounds here and folds below. The first pass
  // still starts at the region start (see noteOn), so ascending playback may
  // travel the region below the marker before entering the loop window.
  if ((loopMode_ == 3 || loopMode_ == 4) && direction_ < 0) {
    if (position_ >= loopStartFrame_ && position_ < endFrame_) return true;
  } else if (position_ >= (timingLooped_ ? loopStartFrame_ : startFrame_) && position_ < endFrame_) {
    return true;
  }
  if (loopMode_ == 0) return false;
  wrap();
  return true;
}

void SampleVoice::noteOff() {
  if (active_) post_.noteOff();
}

void SampleVoice::kill() {
  // Hard cuts click: arm the same short fade the retrigger uses, so the
  // voice keeps rendering a decaying tail from its last output. The
  // transition state is intentionally NOT cleared here - stopImmediately()
  // does that when the transport needs an immediate stop.
  if (active_ && (lastOutput_[0] != 0.0f || lastOutput_[1] != 0.0f)) transitionPending_ = true;
  active_ = false;
  rampRemaining_ = 0;
  stretch_.reset();
  post_.kill();
}

void SampleVoice::stopImmediately() {
  kill();
  lastOutput_[0] = lastOutput_[1] = 0.0f;
  transitionPending_ = false;
  transitionFrames_ = 0;
}

void SampleVoice::finishFrame(float* stereo) {
  if (rampRemaining_) {
    float blend = (float)(rampFrames_ - rampRemaining_ + 1) / rampFrames_;
    for (int channel = 0; channel < 2; ++channel)
      stereo[channel] = rampFrom_[channel] + (stereo[channel] - rampFrom_[channel]) * blend;
    --rampRemaining_;
  }
  lastOutput_[0] = stereo[0];
  lastOutput_[1] = stereo[1];
}

void SampleVoice::renderTailFrame(float* frame) {
  // Linear fade from the output captured at the cut: the first tail frame
  // repeats the cut output (weight 1.0, continuous with lastOutput_) and
  // the last one is silence.
  const float weight = float(transitionFrames_ - 1) / float(transitionLength_ - 1);
  frame[0] = tailFrom_[0] * weight;
  frame[1] = tailFrom_[1] * weight;
  lastOutput_[0] = frame[0];
  lastOutput_[1] = frame[1];
  --transitionFrames_;
}

template <bool Filter, bool UnityEnvelope>
size_t SampleVoice::renderPlain(float* output, size_t frames) {
  size_t rendered = 0;
  const bool mono = sample_->channels == 1;
  for (size_t i = 0; i < frames; ++i) {
    rendered = i;
    if constexpr (!UnityEnvelope) {
      if (!post_.envelopeActive()) { kill(); break; }
    }
    output[i * 2] = sampleAt(position_, 0);
    output[i * 2 + 1] = mono ? output[i * 2] : sampleAt(position_, 1);
    post_.processStereo<Filter, UnityEnvelope>(output + i * 2);
    finishFrame(output + i * 2);
    if (!advancePosition()) { kill(); break; }
    if constexpr (!UnityEnvelope) {
      if (!post_.envelopeActive()) { kill(); break; }
    }
  }
  return rendered;
}

void SampleVoice::render(float* output, size_t frames) {
  memset(output, 0, frames * 2 * sizeof(float));
  if (!sample_ || !sample_->data) return;

  // Tail-only path: the voice was killed (or stopped) but a de-click fade
  // is still pending. Convert the pending arm into a fade from the last
  // rendered output down to silence - no sample reads, no envelope.
  if (!active_) {
    if (!transitionPending_ && !transitionFrames_) return;
    if (transitionPending_) {
      tailFrom_[0] = lastOutput_[0];
      tailFrom_[1] = lastOutput_[1];
      transitionFrames_ = (tailFrom_[0] != 0.0f || tailFrom_[1] != 0.0f)
                              ? transitionLength_
                              : 0;
      transitionPending_ = false;
    }
    for (size_t i = 0; i < frames && transitionFrames_; i++) {
      renderTailFrame(output + i * 2);
    }
    return;
  }

  if (timingPaused_) {
    // Keep the source still while letting the note's envelope release.
    for (size_t i = 0; i < frames; ++i) {
      float* frame = output + i * 2;
      post_.processStereo(frame);
      const float weight = pauseRemaining_ ? (float)--pauseRemaining_ / rampFrames_ : 0.0f;
      frame[0] = lastOutput_[0] = pauseFrom_[0] * weight;
      frame[1] = lastOutput_[1] = pauseFrom_[1] * weight;
      if (!post_.envelopeActive() && !pauseRemaining_) { kill(); break; }
    }
    return;
  }

  if (useStretch_) {
    // The stretch processor owns the read pointer and applies its own
    // windowing; the loop X-fade is bypassed for this voice entirely.
    // Stretch enabled mid-note (per-tick configure, no new noteOn): prime now.
    if (!stretch_.primed()) stretch_.noteOn();
    if (!stretch_.process(output, frames)) { kill(); return; }
    // One-shot material: when the stretcher has drained its tail the note is
    // over, matching the plain path's behavior on source exhaustion.
    const bool stretchDone = !stretch_.active();
    size_t rendered = frames;
    for (size_t i = 0; i < frames; i++) {
      if (!post_.envelopeActive()) { kill(); rendered = i; break; }
      post_.processStereo(output + i * 2);
      finishFrame(output + i * 2);
    }
    // The stretcher filled the whole buffer with raw audio before the
    // envelope pass; zero whatever the envelope pass did not reach so a
    // mid-buffer kill cannot leak un-enveloped material. A kill here arms
    // the de-click tail, which continues in the buffer from the kill frame
    // BEFORE the memset - the tail must not be erased by the zeroing.
    if (rendered < frames) {
      size_t tailEnd = rendered;
      if (transitionPending_) {
        tailFrom_[0] = lastOutput_[0];
        tailFrom_[1] = lastOutput_[1];
        transitionFrames_ = (tailFrom_[0] != 0.0f || tailFrom_[1] != 0.0f)
                                ? transitionLength_
                                : 0;
        transitionPending_ = false;
      }
      for (size_t i = rendered; i < frames && transitionFrames_; i++) {
        renderTailFrame(output + i * 2);
        tailEnd = i + 1;
      }
      memset(output + tailEnd * 2, 0, (frames - tailEnd) * 2 * sizeof(float));
    } else if (stretchDone) {
      kill();
    }
    if (!post_.envelopeActive()) kill();
    return;
  }

  size_t loopRendered = 0;
  const bool monoSource = sample_->channels == 1;
  const bool beat = algorithm_ == SampleAlgorithm::Beat || algorithm_ == SampleAlgorithm::BeatPro;
  if (!beat && !timingGrains_ && !granular_ && !xfade_.enabled() && !xfade_.active()) {
    if (post_.filterActive()) {
      loopRendered = post_.unityEnvelope() ? renderPlain<true, true>(output, frames)
                                          : renderPlain<true, false>(output, frames);
    } else {
      loopRendered = post_.unityEnvelope() ? renderPlain<false, true>(output, frames)
                                          : renderPlain<false, false>(output, frames);
    }
  } else for (size_t i = 0; i < frames; i++) {
    loopRendered = i;
    if (!post_.envelopeActive()) { kill(); break; }
    if (beat || timingGrains_) {
      if (beat) {
        beatFrame(output + i * 2);
      } else {
        timingGrainFrame(output + i * 2, position_, direction_ * pitchStep_, grainHop_,
                         algorithm_ == SampleAlgorithm::Wsola,
                         [this](double pos, int ch) { return grainSampleAt(pos, ch); });
      }
      post_.processStereo(output + i * 2);
      finishFrame(output + i * 2);
      if (!advancePosition()) { kill(); break; }
    } else if (granular_) {
      // The granular path windows its own grains; the loop X-fade is not
      // applied here (its windowing already softens the wrap).
      for (int grain = 0; grain < 2; ++grain) {
        if (grainAge_[grain] >= grainSize_) {
          grainPosition_[grain] = nextGrainPosition_;
          nextGrainPosition_ += direction_ * step_ * grainHop_ * timeStretch_;
          grainAge_[grain] = 0;
          if (loopMode_ == 0 && (reverse_
            ? nextGrainPosition_ <= startFrame_
            : nextGrainPosition_ >= endFrame_)) grainExhausted_ = true;
        }
      }
      float windows[2];
      for (int grain = 0; grain < 2; ++grain) {
        windows[grain] = grainWindowAt(grainAge_[grain], grainWindowScale_);
      }
      const int channels = monoSource ? 1 : 2;
      for (int channel = 0; channel < channels; ++channel) {
        float value = 0.0f;
        for (int grain = 0; grain < 2; ++grain) {
          value += grainSampleAt(grainPosition_[grain] + direction_ * step_ * grainAge_[grain], channel) * windows[grain];
        }
        output[i * 2 + channel] = value;
      }
      if (monoSource) output[i * 2 + 1] = output[i * 2];
      post_.processStereo(output + i * 2);
      finishFrame(output + i * 2);
      grainAge_[0]++;
      grainAge_[1]++;
      double head0 = grainPosition_[0] + direction_ * step_ * grainAge_[0];
      double head1 = grainPosition_[1] + direction_ * step_ * grainAge_[1];
      if (grainExhausted_ && loopMode_ == 0 &&
          (reverse_ ? head0 <= startFrame_ && head1 <= startFrame_
                    : head0 >= endFrame_ && head1 >= endFrame_)) { kill(); break; }
    } else if (xfade_.active()) {
      // Inside the fade window: 2 lookups per channel (outgoing + incoming
      // pass), mixed with the equal-power gains. The outgoing pointer keeps
      // stepping across the seam; the incoming pointer steps from the wrap
      // target. When the fade completes the incoming pointer becomes the
      // main playhead - the handover sample is the pure incoming read, so
      // the plain path resumes without a step.
      if (monoSource) {
        const float sample = xfade_.sample(
          [this](double pos, int ch) { return sampleAt(pos, ch); }, 0);
        output[i * 2] = output[i * 2 + 1] = sample;
      } else {
        for (int channel = 0; channel < 2; channel++) {
          output[i * 2 + channel] =
            xfade_.sample([this](double pos, int ch) { return sampleAt(pos, ch); }, channel);
        }
      }
      post_.processStereo(output + i * 2);
      finishFrame(output + i * 2);
      if (!xfade_.advance(step_)) finishLoopFade();
    } else if (xfade_.enabled() &&
               (loopMode_ == 3 || loopMode_ == 4 || direction_ > 0) &&
               seamDistance() <= (double)xfade_.length()) {
      // Playhead entered the fade window: arm the crossfade. The outgoing
      // pass keeps its natural trajectory to the seam (it is NOT folded
      // here - sampleAt clamps interpolation at the region bounds, which is
      // the same material the fold would read through). The incoming pass
      // is phase-aligned with the outgoing: at mix index i the outgoing
      // reads seam - fadeLen + i while the incoming reads wrapTarget + i *
      // inDir, so the handover at the seam lands exactly on the position
      // the plain path would have produced - the fade removes the
      // discontinuity without shifting the loop.
      //
      // The arm iteration itself renders the mix at the initial phase,
      // which is (almost) the pure outgoing read: identical to the plain
      // path's sample, so no frame is dropped or duplicated at the window
      // edge. Descending forward loops (legacy Start > End markers) are
      // excluded: their wrap target sits at the region end, and the
      // phase-aligned incoming pass would read past the sample data.
      // Modes 3/4 arm through the same branch: the arm condition keys on
      // the wrap geometry (ping-pong or forward travel), not the mode
      // number, so the X-fade variants reuse the plain modes' math.
      const double fadeLen = (double)xfade_.length();
      const double remaining = seamDistance();
      const double phase = fadeLen - remaining;
      double incomingPos;
      int incomingDir;
      if (loopMode_ == 1 || loopMode_ == 2) {
        // Forward wrap: incoming restarts at the loop start marker.
        incomingPos = loopStartFrame_ + phase;
        incomingDir = 1;
      } else if (direction_ > 0) {
        // Ping-pong turn at the region end: incoming reads backwards from
        // the last frame, mirroring the fold position_ = last - overshoot.
        incomingPos = endFrame_ - 1.0 - phase;
        incomingDir = -1;
      } else {
        // Ping-pong turn at the loop start marker: incoming reads forwards
        // from the marker, mirroring position_ = first + (first - pos).
        incomingPos = loopStartFrame_ + phase;
        incomingDir = 1;
      }
      if (algorithm_ != SampleAlgorithm::Legacy) {
        // Blend toward the actual post-fade read, including any fractional
        // or multi-frame stride across the seam. Do not play ahead of it.
        incomingPos = position_ + direction_ * std::max(1.0, std::ceil(remaining / step_)) * step_;
        incomingDir = direction_;
        if (incomingPos < loopStartFrame_ || incomingPos >= endFrame_)
          foldTimingPosition(incomingPos, incomingDir);
        incomingDir = 0;
      }
      xfade_.arm(position_, incomingPos, direction_, incomingDir, phase);
      if (monoSource) {
        const float sample = xfade_.sample(
          [this](double pos, int ch) { return sampleAt(pos, ch); }, 0);
        output[i * 2] = output[i * 2 + 1] = sample;
      } else {
        for (int channel = 0; channel < 2; channel++) {
          output[i * 2 + channel] =
            xfade_.sample([this](double pos, int ch) { return sampleAt(pos, ch); }, channel);
        }
      }
      post_.processStereo(output + i * 2);
      finishFrame(output + i * 2);
      if (!xfade_.advance(step_)) finishLoopFade();
    } else {
      output[i * 2] = sampleAt(position_, 0);
      output[i * 2 + 1] = monoSource ? output[i * 2] : sampleAt(position_, 1);
      post_.processStereo(output + i * 2);
      finishFrame(output + i * 2);
      if (!advancePosition()) { kill(); break; }
    }
    if (!post_.envelopeActive()) { kill(); break; }
  }
  // A kill inside the loop arms the de-click tail; continue it in this
  // buffer so the fade starts at the cut instead of after a zero gap.
  if (!active_ && (transitionPending_ || transitionFrames_)) {
    if (transitionPending_) {
      tailFrom_[0] = lastOutput_[0];
      tailFrom_[1] = lastOutput_[1];
      transitionFrames_ = (tailFrom_[0] != 0.0f || tailFrom_[1] != 0.0f)
                              ? transitionLength_
                              : 0;
      transitionPending_ = false;
    }
    for (size_t i = loopRendered; i < frames && transitionFrames_; i++) {
      renderTailFrame(output + i * 2);
    }
  }
}

void samplePrepareBeatOnsets(InstrumentSampler* sample) {
  auto& timing = sample->timing;
  timing.beatOnsetCount = 0;
  const auto mode = sampleNormalizeAlgorithm(timing.algorithm);
  if ((mode != SampleAlgorithm::Beat && mode != SampleAlgorithm::BeatPro) ||
      !sample->data || !sample->channels) return;

  // A small peak-envelope detector avoids FFT scratch allocations on load.
  // Follow the louder channel, so antiphase stereo attacks remain detectable.
  const uint32_t block = std::max(1u, sample->sampleRate / 500);
  const uint32_t separation = std::max(1u, sample->sampleRate / 32);
  float envelope = 0;
  uint32_t lastOnset = 0;
  for (uint32_t first = 0; first < sample->frameCount;) {
    const uint32_t end = first + std::min(block, sample->frameCount - first);
    const float threshold = std::max(256.0f, envelope * 2.3f);
    int peak = 0;
    uint32_t crossing = end;
    for (uint32_t f = first; f < end; ++f) {
      const int16_t* pcm = sample->data + (size_t)f * sample->channels;
      const int magnitude = sample->channels == 1 ? abs(pcm[0]) : std::max(abs(pcm[0]), abs(pcm[1]));
      if (magnitude > threshold && crossing == end) crossing = f;
      peak = std::max(peak, magnitude);
    }
    if (crossing != end && crossing - lastOnset >= separation) {
      timing.beatOnsets[timing.beatOnsetCount++] = crossing;
      lastOnset = crossing;
      if (timing.beatOnsetCount == PROJECT_SAMPLE_MAX_SLICES) break;
    }
    envelope = std::max((float)peak, envelope * 0.86f);
    first = end;
  }
}

static uint16_t readU16(FILE* file, bool* ok) {
  uint8_t bytes[2];
  if (fread(bytes, 1, 2, file) != 2) { *ok = false; return 0; }
  return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t readU32(FILE* file, bool* ok) {
  uint8_t bytes[4];
  if (fread(bytes, 1, 4, file) != 4) { *ok = false; return 0; }
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

int sampleLoadWav16FileCues(FILE* file, const char* path, InstrumentSample* sample,
                            uint32_t* outCueFrames, uint8_t* outCueCount,
                            char* error, size_t errorSize) {
  if (outCueCount) *outCueCount = 0;
  if (!file) { snprintf(error, errorSize, "Cannot open WAV"); return 1; }
  char id[4];
  bool ok = fread(id, 1, 4, file) == 4 && !memcmp(id, "RIFF", 4);
  (void)readU32(file, &ok);
  ok = ok && fread(id, 1, 4, file) == 4 && !memcmp(id, "WAVE", 4);
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t sampleRate = 0, dataSize = 0;
  long dataOffset = 0;
  // Cue points from the `cue ` chunk (Phase 4): sample offsets of the slice
  // starts written by sampleSaveWav16WithCues. Only the first 64 are kept
  // (the slice cap); the rest are skipped.
  uint32_t cueFrames[PROJECT_SAMPLE_MAX_SLICES];
  uint8_t cueCount = 0;

  while (ok && fread(id, 1, 4, file) == 4) {
    uint32_t size = readU32(file, &ok);
    if (!ok) break;
    long nextChunk = ftell(file) + size + (size & 1);
    if (!memcmp(id, "fmt ", 4) && size >= 16) {
      format = readU16(file, &ok);
      channels = readU16(file, &ok);
      sampleRate = readU32(file, &ok);
      (void)readU32(file, &ok);
      (void)readU16(file, &ok);
      bits = readU16(file, &ok);
    } else if (!memcmp(id, "data", 4)) {
      dataOffset = ftell(file);
      dataSize = size;
    } else if (!memcmp(id, "cue ", 4) && size >= 4) {
      const uint32_t numCues = readU32(file, &ok);
      // Each cue point record is 24 bytes: dwIdentifier, dwPosition,
      // fccChunk (4 bytes), dwChunkStart, dwBlockStart, dwSampleOffset.
      // Only dwSampleOffset (the last field) matters here.
      for (uint32_t i = 0; ok && i < numCues && i < 1024; ++i) {
        (void)readU32(file, &ok);  // cue id
        (void)readU32(file, &ok);  // position
        if (fread(id, 1, 4, file) != 4) ok = false;
        (void)readU32(file, &ok);  // chunk start
        (void)readU32(file, &ok);  // block start
        const uint32_t offset = readU32(file, &ok);
        if (ok && cueCount < PROJECT_SAMPLE_MAX_SLICES) {
          cueFrames[cueCount++] = offset;
        }
      }
    }
    if (fseek(file, nextChunk, SEEK_SET) != 0) ok = false;
  }

  if (!ok || format != 1 || (channels != 1 && channels != 2) ||
      (bits != 8 && bits != 16 && bits != 24) || sampleRate < 1000 ||
      sampleRate > 192000 || !dataOffset) {
    snprintf(error, errorSize, "Need PCM8/16/24 mono/stereo WAV");
    return 1;
  }
  uint32_t bytesPerSample = bits / 8;
  uint32_t frames = dataSize / (channels * bytesPerSample);
  if (frames == 0 || dataSize > 64U * 1024U * 1024U) {
    snprintf(error, errorSize, "WAV empty or over 64 MB");
    return 1;
  }
  uint32_t sampleCount = frames * channels;
  int16_t* data = (int16_t*)malloc(sampleCount * sizeof(int16_t));
  if (!data || fseek(file, dataOffset, SEEK_SET) != 0) {
    free(data);
    snprintf(error, errorSize, "Cannot read WAV data");
    return 1;
  }
  if (bits == 16) {
    ok = fread(data, sizeof(int16_t), sampleCount, file) == sampleCount;
  } else if (bits == 8) {
    for (uint32_t i = 0; i < sampleCount; i++) {
      int value = fgetc(file);
      if (value == EOF) { ok = false; break; }
      data[i] = (int16_t)((value - 128) << 8);
    }
  } else {
    // 24-bit: three little-endian bytes per sample, sign-extended and
    // scaled down to 16-bit (arithmetic shift keeps the full-scale range).
    for (uint32_t i = 0; i < sampleCount; i++) {
      uint8_t bytes[3];
      if (fread(bytes, 1, 3, file) != 3) { ok = false; break; }
      int32_t value = (int32_t)(bytes[0] | (bytes[1] << 8) | (bytes[2] << 16));
      if (value & 0x800000) value |= 0xFF000000;
      data[i] = (int16_t)(value >> 8);
    }
  }
  if (!ok) {
    free(data);
    snprintf(error, errorSize, "Cannot read WAV data");
    return 1;
  }
  free(sample->data);
  sample->data = data;
  sample->frameCount = frames;
  sample->sampleRate = sampleRate;
  sample->channels = (uint8_t)channels;
  sampleStorePath(path, sample);
  if (outCueFrames && outCueCount) {
    memcpy(outCueFrames, cueFrames, cueCount * sizeof(uint32_t));
    *outCueCount = cueCount;
  }
  error[0] = 0;
  return 0;
}

int sampleLoadWav16File(FILE* file, const char* path, InstrumentSample* sample,
                        char* error, size_t errorSize) {
  return sampleLoadWav16FileCues(file, path, sample, NULL, NULL, error, errorSize);
}

int sampleLoadWav16Cues(const char* path, InstrumentSample* sample,
                        uint32_t* outCueFrames, uint8_t* outCueCount,
                        char* error, size_t errorSize) {
  FILE* file = fopen(path, "rb");
  if (!file) { snprintf(error, errorSize, "Cannot open WAV"); return 1; }
  int result = sampleLoadWav16FileCues(file, path, sample, outCueFrames, outCueCount,
                                       error, errorSize);
  fclose(file);
  return result;
}

int sampleLoadWav16(const char* path, InstrumentSample* sample,
                    char* error, size_t errorSize) {
  return sampleLoadWav16Cues(path, sample, NULL, NULL, error, errorSize);
}

static void writeU16(FILE* file, uint16_t value, bool* ok) {
  uint8_t bytes[2] = { (uint8_t)(value & 0xFF), (uint8_t)(value >> 8) };
  if (fwrite(bytes, 1, 2, file) != 2) *ok = false;
}

static void writeU32(FILE* file, uint32_t value, bool* ok) {
  uint8_t bytes[4] = { (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF),
                       (uint8_t)((value >> 16) & 0xFF), (uint8_t)(value >> 24) };
  if (fwrite(bytes, 1, 4, file) != 4) *ok = false;
}

int sampleSaveWav16WithCues(const InstrumentSample* sample, const char* path,
                            const uint32_t* cueFrames, uint8_t cueCount,
                            char* error, size_t errorSize) {
  if (!sample->data || sample->frameCount == 0) {
    snprintf(error, errorSize, "No sample to save");
    return 1;
  }
  if (strlen(path) > PROJECT_SAMPLE_PATH_LENGTH) {
    snprintf(error, errorSize, "Path too long");
    return 1;
  }
  // Written beside the original and renamed into place once it is on the
  // storage device, so a failed or interrupted save keeps the old WAV (see
  // fileCloseSynced).
#ifdef WEB_BUILD
  const char* writePath = path;
#else
  char tempPath[PROJECT_SAMPLE_PATH_LENGTH + 5];
  snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);
  const char* writePath = tempPath;
#endif
  FILE* file = fopen(writePath, "wb");
  if (!file) {
    snprintf(error, errorSize, "Cannot create WAV");
    return 1;
  }

  const uint16_t channels = sample->channels >= 2 ? 2 : 1;
  const uint16_t bits = 16;
  const uint32_t dataBytes = (uint32_t)sample->frameCount * channels * 2;
  // `cue ` chunk: 4-byte count + 24 bytes per cue point (standard cue
  // record: id, position, "data" chunk id, chunk start, block start,
  // sample offset). Written between fmt and data.
  const uint8_t cues = cueFrames && cueCount ? cueCount : 0;
  const uint32_t cueChunkSize = cues ? 4 + 24u * cues : 0;
  bool ok = true;
  fwrite("RIFF", 1, 4, file);
  writeU32(file, 36 + dataBytes + cueChunkSize, &ok);
  fwrite("WAVE", 1, 4, file);
  fwrite("fmt ", 1, 4, file);
  writeU32(file, 16, &ok);            // fmt chunk size
  writeU16(file, 1, &ok);             // PCM
  writeU16(file, channels, &ok);
  writeU32(file, sample->sampleRate, &ok);
  writeU32(file, sample->sampleRate * channels * 2, &ok); // byte rate
  writeU16(file, channels * 2, &ok);  // block align
  writeU16(file, bits, &ok);
  if (cues) {
    fwrite("cue ", 1, 4, file);
    writeU32(file, cueChunkSize, &ok);
    writeU32(file, cues, &ok);
    for (uint8_t i = 0; ok && i < cues; ++i) {
      writeU32(file, i, &ok);         // dwIdentifier (cue id)
      writeU32(file, 0, &ok);         // dwPosition
      fwrite("data", 1, 4, file);     // fccChunk: cue points into the data chunk
      writeU32(file, 0, &ok);         // dwChunkStart
      writeU32(file, 0, &ok);         // dwBlockStart
      writeU32(file, cueFrames[i], &ok);  // dwSampleOffset
    }
  }
  fwrite("data", 1, 4, file);
  writeU32(file, dataBytes, &ok);
  if (ok && fwrite(sample->data, sizeof(int16_t), (size_t)sample->frameCount * channels, file) !=
      (size_t)sample->frameCount * channels) {
    ok = false;
  }

  if (fileCloseSynced(file, ok ? 0 : 1) != 0) ok = false;
#ifndef WEB_BUILD
  if (ok && fileReplace(tempPath, path) != 0) ok = false;
#endif
  if (!ok) {
    remove(writePath);
    snprintf(error, errorSize, "Cannot write WAV");
    return 1;
  }
  error[0] = 0;
  return 0;
}

int sampleSaveWav16(const InstrumentSample* sample, const char* path,
                    char* error, size_t errorSize) {
  return sampleSaveWav16WithCues(sample, path, NULL, 0, error, errorSize);
}
