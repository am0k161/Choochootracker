// Engine tests for the sample FX: SLI (slice select) and SPL (playback mode,
// formerly SLP). Uses the same fixture pattern as test_lazy_mode.cpp.
#include "doctest.h"
#include "../../chipnomad_lib/project_instruments.h"
#include "../../chipnomad_lib/project.h"
#include "../../chipnomad_lib/synth/sample_voice.h"
#include "../../src/project_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

TEST_SUITE("sample_fx") {

// --- Fixture (same pattern as test_lazy_mode.cpp) --------------------------

namespace {

// Ramp sample: data[i] = i % 32768, so the playback position can be read back
// from the voice cursor directly. One tick = 882 frames (44100 Hz / 50 Hz).
struct EngineSample {
  InstrumentSample s;
  std::vector<int16_t> data;

  void init(uint32_t frames) {
    std::memset(&s, 0, sizeof(s));
    s.frameCount = frames;
    s.channels = 1;
    s.sampleRate = 44100;
    s.start = 0;
    s.end = 255;
    s.speedPercent = 100;
    s.pitch = 0;
    s.filterEnabled = 1;
    s.filterCharacter = 2;
    s.filterMode = 0;
    s.filterSlope24dB = 0;
    s.filterCutoffHz = FILTER_CUTOFF_MAX_HZ;
    s.filterResonance = 0;
    s.attack = 0;
    s.decay = 0;
    s.sustain = 255;
    s.release = 0;
    s.envelopeShape = 0x80;
    data.resize(frames);
    for (uint32_t i = 0; i < frames; ++i) data[i] = (int16_t)(i % 32768);
    s.data = data.data();
  }
};

struct LazyEngine {
  ChipNomadState* state = chipnomadCreate();

  // The sample instrument must exist BEFORE chipnomadInitChips: InitChips
  // copies the project into the audio snapshot, and updateSampleVoices
  // kills voices whose instrument is not a Sample in that snapshot.
  LazyEngine(const InstrumentSample& sample, uint8_t instrument) {
    projectInitAY(&state->project);
    state->project.trackVolume[0] = 100;
    state->project.trackVolume[1] = 0;
    state->project.trackVolume[2] = 0;
    state->project.trackVolume[3] = 0;
    setSampleInstrument(instrument, sample);
    // The sample data buffer is owned by the fixture (std::vector), not by
    // the project: without this, chipnomadDestroy -> projectFree frees the
    // shared data pointer and the vector destructor frees it again.
    state->ownsProjectResources = 0;
    chipnomadInitChips(state, 44100, nullptr);
  }
  ~LazyEngine() { chipnomadDestroy(state); }

  void setSampleInstrument(uint8_t instrument, const InstrumentSample& sample) {
    Instrument& inst = state->project.instruments[instrument];
    getInstrumentFunctions(InstrumentType::Sample).init(&inst);
    inst.volume = 255;
    inst.chip.sample = sample; // copies the struct; data pointer shared
  }

  void render(int frames) {
    std::vector<float> buffer((size_t)frames * 2);
    chipnomadRender(state, buffer.data(), frames);
  }

  SampleVoice* voice() { return state->sampleVoices[0][0]; }
};

// Phrase row helper: root note (48 = octaveSize*4) keeps the playback step at
// 1.0 so cursor math stays frame-exact.
static PhraseRow makeRow(uint8_t note) {
  PhraseRow row;
  std::memset(&row, 0, sizeof(PhraseRow));
  row.note = note;
  row.instrument = 0;
  row.volume = PHRASE_VOLUME_MAX;
  return row;
}

}  // namespace

// --- SLI: slice select overrides note mapping -------------------------------
// Sliced instruments keep the playback step at 1.0 (the note selects the
// slice, not the pitch), so after rendering N frames the cursor sits at
// sliceStart + N.

TEST_CASE("SLI plays the numbered slice regardless of note pitch") {
  EngineSample es;
  es.init(8820); // 10 slices of 882 frames
  // Slicing must be set BEFORE the engine snapshots the project.
  es.s.slice = sampleEncodeSlice(sliceModeEqual, 10);
  LazyEngine engine(es.s, 0);

  // Note 48 maps to slice 8 (48 % 10) without the override; SLI = 01 must
  // play the FIRST slice instead.
  PhraseRow row = makeRow(48);
  row.fx[0][0] = fxSLI;
  row.fx[0][1] = 1;

  chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
  engine.render(400);

  SampleVoice* v = engine.voice();
  REQUIRE(v->active());
  // Slice 0 spans frames 0..881; the cursor is at ~400.
  CHECK(v->playbackFrame() < 882);
}

TEST_CASE("SLI 00 keeps normal note mapping") {
  EngineSample es;
  es.init(8820);
  es.s.slice = sampleEncodeSlice(sliceModeEqual, 10);
  LazyEngine engine(es.s, 0);

  PhraseRow row = makeRow(48); // maps to slice 8 (48 % 10)
  row.fx[0][0] = fxSLI;
  row.fx[0][1] = 0;

  chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
  engine.render(400);

  SampleVoice* v = engine.voice();
  REQUIRE(v->active());
  // Slice 8 spans frames 7056..7937.
  CHECK(v->playbackFrame() >= 7056);
  CHECK(v->playbackFrame() < 7938);
}

TEST_CASE("SLI value wraps past the last slice") {
  EngineSample es;
  es.init(8820);
  es.s.slice = sampleEncodeSlice(sliceModeEqual, 10);
  LazyEngine engine(es.s, 0);

  PhraseRow row = makeRow(48);
  row.fx[0][0] = fxSLI;
  row.fx[0][1] = 11; // wraps to slice 0 (11-1 = 10 % 10)

  chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
  engine.render(400);

  SampleVoice* v = engine.voice();
  REQUIRE(v->active());
  CHECK(v->playbackFrame() < 882);
}

// --- SPL: playback modes -----------------------------------------------------

TEST_CASE("long silent looping sample stays silent through RET and phrase wrap") {
  EngineSample es;
  es.init(44100 * 4);
  std::fill(es.data.begin(), es.data.end(), 0);
  es.s.loopMode = 1;
  LazyEngine engine(es.s, 0);
  PhraseRow row = makeRow(48);
  row.fx[0][0] = fxRET;
  row.fx[0][1] = 1;
  for (Project* project : {&engine.state->project, &engine.state->audioProject}) {
    project->song[0][0] = 0;
    project->chains[0].rows[0].phrase = 0;
    project->phrases[0].rows[0] = row;
  }
  chipnomadQueuePlaybackStartPhrase(engine.state, 0, 0, 0, 1);

  std::vector<float> output(44100 * 2 * 2);
  REQUIRE(chipnomadRender(engine.state, output.data(), 44100 * 2) == 44100 * 2);
  float peak = 0.0f;
  for (float value : output) peak = std::max(peak, std::fabs(value));
  CHECK(peak < 1e-5f);
}

TEST_CASE("phrase loop crossfades a long sample at its retrigger") {
  EngineSample es;
  es.init(44100 * 4);
  std::fill(es.data.begin(), es.data.end(), -16000);
  std::fill(es.data.begin(), es.data.begin() + 1000, 16000);
  es.s.loopMode = 1;
  es.s.filterEnabled = 0;
  LazyEngine engine(es.s, 0);
  PhraseRow row = makeRow(48);
  for (Project* project : {&engine.state->project, &engine.state->audioProject}) {
    project->song[0][0] = 0;
    project->chains[0].rows[0].phrase = 0;
    project->phrases[0].rows[0] = row;
  }
  chipnomadQueuePlaybackStartPhrase(engine.state, 0, 0, 0, 1);

  std::vector<float> output(44100 * 2 * 2);
  REQUIRE(chipnomadRender(engine.state, output.data(), 44100 * 2) == 44100 * 2);
  REQUIRE(engine.voice()->active());
  CHECK(engine.voice()->playbackFrame() < 10000);
  float largestStep = 0.0f;
  for (int frame = 84000; frame < 85000; ++frame)
    largestStep = std::max(largestStep, std::fabs(output[frame * 2] - output[(frame - 1) * 2]));
  CHECK(largestStep < 0.03f);
}

// Non-sliced sample, root note: step 1.0, window 0..8820. After one tick
// (882 frames) the cursor has moved 882 frames from its start.

TEST_CASE("SPL 01 reverse plays the sample backwards") {
  EngineSample es;
  es.init(8820);
  LazyEngine engine(es.s, 0);

  PhraseRow row = makeRow(48);
  row.fx[0][0] = fxSLP;
  row.fx[0][1] = 1; // reverse

  chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
  engine.render(882);

  SampleVoice* v = engine.voice();
  REQUIRE(v->active());
  // Reverse starts at the window end (8819) and one tick has played:
  // 8819 - 882 = 7937. Forward playback would sit near frame 882 instead.
  const double first = v->playbackFrame();
  CHECK(first > 7000);

  // The cursor keeps moving DOWN on the next tick.
  engine.render(882);
  const double second = v->playbackFrame();
  CHECK(second < first);
  CHECK(second > 5000);
}

TEST_CASE("SPL 00 forward keeps ascending playback") {
  EngineSample es;
  es.init(8820);
  LazyEngine engine(es.s, 0);

  PhraseRow row = makeRow(48);
  row.fx[0][0] = fxSLP;
  row.fx[0][1] = 0; // forward

  chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
  engine.render(882);

  SampleVoice* v = engine.voice();
  REQUIRE(v->active());
  const double first = v->playbackFrame();
  CHECK(first < 1000);

  // The cursor keeps moving UP on the next tick.
  engine.render(882);
  CHECK(v->playbackFrame() > first);
}

TEST_CASE("SPL 02 maps to loop and SPL 03 maps to ping-pong") {
  EngineSample es;
  es.init(8820);

  // Loop: voice stays alive past the window end.
  {
    LazyEngine engine(es.s, 0);
    PhraseRow row = makeRow(48);
    row.fx[0][0] = fxSLP;
    row.fx[0][1] = 2; // loop
    chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
    engine.render(882 * 3);
    CHECK(engine.voice()->active());
  }

  // Ping-pong: voice stays alive past the window end too.
  {
    LazyEngine engine(es.s, 0);
    PhraseRow row = makeRow(48);
    row.fx[0][0] = fxSLP;
    row.fx[0][1] = 3; // ping-pong
    chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
    engine.render(882 * 3);
    CHECK(engine.voice()->active());
  }
}

// --- LOOP START marker -------------------------------------------------------
// loopStart is a 0-255 normalized marker (like start/end). When a loop mode is
// active (instrument loopMode != 0 or SPL >= 02) the voice wraps back to the
// LOOP START frame instead of the Region START frame. With no loop the marker
// is inert and playback stays bit-identical. On the 8820-frame fixture,
// loopStart 0x40 maps to frame 2213 (sampleMarkerToStartFrame).

TEST_CASE("loop start is inert when the loop is off") {
  // Two engines, same sample: one with loopStart 0, one with 0x40, both with
  // loopMode 0. The cursor trajectory must be identical (inert marker).
  EngineSample es;
  es.init(8820);

  std::vector<double> baseline;
  {
    LazyEngine engine(es.s, 0);
    PhraseRow row = makeRow(48);
    chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
    for (int tick = 0; tick < 8; ++tick) {
      engine.render(882);
      baseline.push_back(engine.voice()->playbackFrame());
    }
  }

  es.s.loopStart = 0x40;
  std::vector<double> withMarker;
  {
    LazyEngine engine(es.s, 0);
    PhraseRow row = makeRow(48);
    chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
    for (int tick = 0; tick < 8; ++tick) {
      engine.render(882);
      withMarker.push_back(engine.voice()->playbackFrame());
    }
  }

  REQUIRE(baseline.size() == withMarker.size());
  for (size_t i = 0; i < baseline.size(); ++i) {
    CHECK(baseline[i] == withMarker[i]);
  }
}

TEST_CASE("forward loop wraps to the loop start marker") {
  EngineSample es;
  es.init(8820);
  es.s.loopMode = 1;
  es.s.loopStart = 0x40; // frame 2213 of 8820
  LazyEngine engine(es.s, 0);

  PhraseRow row = makeRow(48);
  chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
  // One tick = 882 frames at 44100 Hz / 50 Hz. The region is the whole
  // sample, so the first wrap happens during tick 10.
  for (int tick = 0; tick < 9; ++tick) engine.render(882);
  const double before = engine.voice()->playbackFrame();
  CHECK(before > 7000.0); // still in the first pass, near the end
  engine.render(882);
  const double after = engine.voice()->playbackFrame();
  // Wrapped back to the LOOP START frame (2213), not the Region START (0).
  CHECK(after >= 2213.0);
  CHECK(after < 2213.0 + 882.0);
}

TEST_CASE("ping-pong bounces at the loop start marker") {
  EngineSample es;
  es.init(8820);
  es.s.loopMode = 2;
  es.s.loopStart = 0x40;
  LazyEngine engine(es.s, 0);

  PhraseRow row = makeRow(48);
  chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
  // Play until the cursor has passed the end and reversed: after enough
  // ticks the cursor must bounce at the LOOP START frame, never reaching
  // the region start (0).
  double minSeen = 1e18;
  for (int tick = 0; tick < 40; ++tick) {
    engine.render(882);
    const double frame = engine.voice()->playbackFrame();
    if (tick >= 12) minSeen = frame < minSeen ? frame : minSeen;
  }
  // The loop window is [2213, 8820); the bounce lower bound is 2213.
  CHECK(minSeen >= 2213.0 - 1.0);
  // And the cursor did travel up to the top at least once (sanity).
  CHECK(minSeen < 8820.0);
}

TEST_CASE("region end moved below the loop start clamps it") {
  EngineSample es;
  es.init(8820);
  es.s.loopMode = 1;
  es.s.loopStart = 0x80; // frame 4426
  // Region END at 0x40 puts the region end frame (2239) below the marker
  // frame (4426): the voice must clamp the wrap point into the region.
  es.s.end = 0x40;
  LazyEngine engine(es.s, 0);

  PhraseRow row = makeRow(48);
  chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
  // The clamped loop window is [0, 2239): the voice must keep looping
  // inside it instead of wrapping to the unclamped marker frame.
  bool wrapped = false;
  for (int tick = 0; tick < 12; ++tick) {
    engine.render(882);
    const double frame = engine.voice()->playbackFrame();
    if (tick > 0 && frame < 2239.0) wrapped = true;
    CHECK(frame < 2239.0);
  }
  CHECK(wrapped);
}

TEST_CASE("SPL 03 ping-pong respects the loop start marker") {
  EngineSample es;
  es.init(8820);
  es.s.loopMode = 0; // instrument loop off; SPL must override
  es.s.loopStart = 0x40;
  LazyEngine engine(es.s, 0);

  PhraseRow row = makeRow(48);
  row.fx[0][0] = fxSLP;
  row.fx[0][1] = 3; // ping-pong via SPL
  chipnomadQueuePlaybackStartPhraseRow(engine.state, 0, &row);
  double minSeen = 1e18;
  for (int tick = 0; tick < 40; ++tick) {
    engine.render(882);
    const double frame = engine.voice()->playbackFrame();
    if (tick >= 12) minSeen = frame < minSeen ? frame : minSeen;
  }
  // SPL 03 activates the loop, so the bounce lower bound is the marker
  // frame (2213), not the region start (0).
  CHECK(minSeen >= 2213.0 - 1.0);
  CHECK(minSeen < 8820.0);
}

TEST_CASE("sample loop start survives save and load; missing field is zero") {
  Project saved, loaded;
  projectInit(&saved);
  projectInit(&loaded);
  saved.chipsCount = 1;
  saved.tracksCount = 1;
  saved.chipType = ChipType::AY;
  std::strcpy(saved.pitchTable.name, "Test");
  saved.pitchTable.length = 1;
  std::strcpy(saved.pitchTable.noteNames[0], "C-4");
  saved.pitchTable.values[0] = 1000;
  getInstrumentFunctions(InstrumentType::Sample).init(&saved.instruments[0]);
  saved.instruments[0].chip.sample.loopStart = 0x40;
  const char* path = "build/tests/sample_loop_start_io.cct";
  REQUIRE(projectSave(&saved, path) == 0);
  INFO(projectFileError);
  REQUIRE(projectLoad(&loaded, path) == 0);
  CHECK(loaded.instruments[0].type == InstrumentType::Sample);
  CHECK(loaded.instruments[0].chip.sample.loopStart == 0x40);

  // A file without the field loads with loopStart 0 (Region START wrap).
  FILE* in = std::fopen(path, "r");
  REQUIRE(in != nullptr);
  const char* stripped = "build/tests/sample_loop_start_missing.cct";
  FILE* out = std::fopen(stripped, "w");
  REQUIRE(out != nullptr);
  char line[512];
  while (std::fgets(line, sizeof(line), in)) {
    if (std::strncmp(line, "- Sample loop start:", 20) == 0) continue;
    std::fputs(line, out);
  }
  std::fclose(in);
  std::fclose(out);

  Project missing;
  projectInit(&missing);
  REQUIRE(projectLoad(&missing, stripped) == 0);
  CHECK(missing.instruments[0].type == InstrumentType::Sample);
  CHECK(missing.instruments[0].chip.sample.loopStart == 0);
  std::remove(path);
  std::remove(stripped);
}

}  // TEST_SUITE("sample_fx")
