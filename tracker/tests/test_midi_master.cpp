#include "doctest.h"
#include "chipnomad_lib.h"
#include "playback.h"
#include "pitch_table_utils.h"
#include "midi/midi_router.h"
#include <vector>

namespace {
std::vector<MidiEvent> events;
void send(void*, const MidiEvent* event, uint64_t due) {
  if (event->type < 0xf8) return; // This test observes clock/transport only.
  MidiEvent copy = *event;
  copy.timestampMicros = due;
  events.push_back(copy);
}
uint64_t now(void*) { return 1000000; }
struct MasterFixture {
  MidiBackend backend{};
  ChipNomadState* state;
  MasterFixture() {
    midiRouterSetOutputDelayMs(0);
    backend.scheduleOutput = send;
    backend.nowMicros = now;
    backend.hasClockOutput = 1;
    midiRouterSetBackend(&backend);
    state = chipnomadCreate();
    state->project.tickRate = 48; // 120 BPM
    state->project.chipType = ChipType::AY;
    state->project.chipsCount = 1;
    state->project.tracksCount = projectGetTotalTracks(&state->project);
    state->project.chipSetup.ay = {1773400, 0, StereoModeAY::ABC, 50, 0};
    calculatePitchTableAY(&state->project);
    state->project.song[0][0] = 0;
    state->project.chains[0].rows[0].phrase = 0;
    chipnomadInitChips(state, 48000, nullptr);
    playbackInit(&state->playbackState, &state->project);
    events.clear();
  }
  ~MasterFixture() {
    midiRouterSetOutputDelayMs(0);
    chipnomadDestroy(state);
    midiRouterSetBackend(nullptr);
  }
};
}

TEST_CASE_FIXTURE(MasterFixture, "MIDI output delay aligns scheduled clock and leaves panic immediate") {
  midiRouterSetOutputDelayMs(40);
  midiRouterSetSyncOutput(state->midiRouter, 1, 1);
  midiRouterMasterTick(state->midiRouter, 1, 1000000);
  REQUIRE(events.size() == 2);
  CHECK(events[0].timestampMicros == 1040000);
  CHECK(events[1].timestampMicros == 1040000);
  midiRouterPanic(state->midiRouter);
  REQUIRE(events.size() == 3);
  CHECK(events[2].type == 0xfc);
  CHECK(events[2].timestampMicros == 1000000);
}

TEST_CASE_FIXTURE(MasterFixture, "MIDI master follows audio ticks and transport") {
  midiRouterSetSyncOutput(state->midiRouter, 1, 1);
  std::vector<float> audio(48000);
  chipnomadRender(state, audio.data(), 1000);
  CHECK(events.empty());
  chipnomadQueuePlaybackPreviewNote(state, 0, 60, 0);
  chipnomadRender(state, audio.data(), 1000);
  CHECK(events.empty()); // Audition must not start external gear.
  chipnomadQueuePlaybackStop(state);
  chipnomadRender(state, audio.data(), 1000);
  events.clear();
  REQUIRE(chipnomadQueuePlaybackStartSongAtTime(state, 0, 0, 1, 0));
  chipnomadRender(state, audio.data(), 24000); // Half a second = 24 clocks.
  REQUIRE(events.size() == 25);
  CHECK(events[0].type == 0xfa);
  for (int i = 0; i < 24; ++i) {
    CHECK(events[i + 1].type == 0xf8);
    CHECK(events[i + 1].timestampMicros == 1000000 + (uint64_t)(i * 1000000.0 / 48));
  }
  events.clear();
  chipnomadQueuePlaybackStop(state);
  chipnomadRender(state, audio.data(), 1000);
  REQUIRE(events.size() == 1);
  CHECK(events[0].type == 0xfc);
  events.clear();
  chipnomadRender(state, audio.data(), 1000);
  CHECK(events.empty());
}

TEST_CASE_FIXTURE(MasterFixture, "MIDI master stops on disable and panic; slave never echoes") {
  auto* router = state->midiRouter;
  midiRouterSetSyncOutput(router, 1, 1);
  midiRouterSetClockMode(router, MidiClockMode::off);
  midiRouterMasterTick(router, 1, 10);
  midiRouterSetClockMode(router, MidiClockMode::slave);
  midiRouterMasterTick(router, 1, 20);
  midiRouterMasterTick(router, 1, 30);
  REQUIRE(events.size() == 3);
  CHECK(events[2].type == 0xfc);
  midiRouterSetSyncOutput(router, 1, 1);
  midiRouterSetClockMode(router, MidiClockMode::off);
  midiRouterMasterTick(router, 1, 40);
  events.clear();
  midiRouterPanic(router);
  REQUIRE(!events.empty());
  CHECK(events[0].type == 0xfc);
}

TEST_CASE_FIXTURE(MasterFixture, "MIDI clock and transport switches are independent") {
  auto* router = state->midiRouter;
  midiRouterMasterTick(router, 1, 0);
  CHECK(events.empty()); // Defaults are off.
  midiRouterSetSyncOutput(router, 1, 0);
  midiRouterMasterTick(router, 1, 10);
  midiRouterMasterTick(router, 0, 20);
  REQUIRE(events.size() == 1);
  CHECK(events[0].type == 0xf8);
  events.clear();
  midiRouterSetSyncOutput(router, 0, 1);
  midiRouterMasterTick(router, 1, 30);
  midiRouterMasterTick(router, 1, 40);
  midiRouterSetSyncOutput(router, 0, 0);
  midiRouterMasterTick(router, 1, 50);
  midiRouterMasterTick(router, 0, 60);
  REQUIRE(events.size() == 2);
  CHECK(events[0].type == 0xfa);
  CHECK(events[1].type == 0xfc);
  events.clear();
  midiRouterSetSyncOutput(router, 1, 1);
  midiRouterSetClockMode(router, MidiClockMode::slave);
  midiRouterMasterTick(router, 1, 70);
  CHECK(events.empty());
  midiRouterSetClockMode(router, MidiClockMode::off);
  midiRouterMasterTick(router, 1, 80);
  REQUIRE(events.size() == 2); // Switches survive entering/leaving slave mode.
  CHECK(events[0].type == 0xfa);
  CHECK(events[1].type == 0xf8);
}

TEST_CASE_FIXTURE(MasterFixture, "MIDI clock follows engine tempo changes without restarting transport") {
  midiRouterSetSyncOutput(state->midiRouter, 1, 1);
  std::vector<float> audio(48000);
  chipnomadQueuePlaybackStartSong(state, 0, 0, 1);
  chipnomadRender(state, audio.data(), 24000);
  REQUIRE(events.size() == 25);
  events.clear();
  state->project.tickRate = 96; // 240 BPM, as applied by tempo editing or Link.
  REQUIRE(chipnomadQueueProjectRefresh(state));
  chipnomadRender(state, audio.data(), 24000);
  REQUIRE(events.size() == 48);
  for (int i = 0; i < 48; ++i) {
    CHECK(events[i].type == 0xf8);
    CHECK(events[i].timestampMicros == 1000000 + (uint64_t)(i * 1000000.0 / 96));
  }
}
