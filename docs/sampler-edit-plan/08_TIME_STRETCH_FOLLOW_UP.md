# Sampler time-stretch follow-up

- **BeatSync UI.** Explore presenting musical duration as a sync toggle plus a value while keeping percent-based Speed, SSP and Speed modulation available when sync is off. Keep the current project fields readable.
- **Bake Repitch into Pitch.** Explore converting the current rate into Semi/Fine and turning off tempo sync in one action. Define rounding, range overflow and effects on an existing Pitch value before adding the action.
- **User slices and musical Stretch.** Define whether the selected duration applies to each triggered slice or to the full sample. Keep Beat's private onset map separate from editable slice markers.
- **Spectral at extreme compression.** In `stretch_processor.cpp`, check output duration when a render chunk would request over 8192 input frames and both chunk caps are in play. Adjust chunk sizing to preserve the requested ratio. Also correct the `process()` comment: a completed drain clears `active()` and can still return `true`.
- **Spectral region modulation.** `StretchProcessor::configureWindow()` invalidates priming when either bound changes, so an in-range marker edit can jump to the start of the window. Preserve the current source cursor when the new region still contains it; test the audible output during a held note.
- **PCM replacement lifetime.** A paused WAV replacement can free PCM before the audio snapshot adopts the new pointer at a tick boundary. Keep the previous buffer alive until the renderer acknowledges the new snapshot.
- **Hardware and audio quality.** Compare `Beat`, `Beat Pro`, `Granular` and `Spectral` on sparse and dense drum loops. Record underruns and callback times on Miyoo Mini Plus and Windows. The offline `benchmark_song --sampler-stretch` comparison cannot measure device underruns.
- **Render caching.** Revisit a bounded cache only if real-device measurements show sustained load is still a problem. Include invalidation for PCM edits, tempo and source-region changes.
