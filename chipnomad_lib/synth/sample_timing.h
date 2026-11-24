#ifndef CHOOCHOO_SAMPLE_TIMING_H
#define CHOOCHOO_SAMPLE_TIMING_H

#include <stdint.h>

// Zero speed is a stopped transport. A valid musical target takes precedence.
static inline double sampleTargetSeconds(double sourceSeconds, uint8_t stretchMode,
                                         float tickRateHz, float speedPercent) {
  static constexpr uint16_t ticks[] = {0, 24, 48, 96, 192, 384, 768};
  if (stretchMode && stretchMode < sizeof(ticks) / sizeof(ticks[0]))
    return ticks[stretchMode] / (tickRateHz > 0.0f ? (double)tickRateHz : 50.0);
  return speedPercent > 0 ? sourceSeconds * 100.0 / speedPercent : 0.0;
}

#endif
