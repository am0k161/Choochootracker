#include "doctest.h"
#include "midi/midi_router.h"

#include <vector>
#include <cstdint>

// Router-level tests through a hand-built fake MidiBackend: this is the
// "testable with a fake backend" requirement from the upstream review, and
// closes a real gap - none of this logic (legato, panic, Program/Bank
// caching) could be exercised by the automated suite before, since the real
// desktop backend is only live under DESKTOP_BUILD, which the test build
// never defines.

namespace {

struct SentMidiEvent { uint8_t type, channel, data1, data2; };
std::vector<SentMidiEvent> g_sent;
std::vector<MidiEvent> g_incoming;
size_t g_incomingIndex;
uint64_t g_fakeNowMicros = 12345;

void resetFake() {
  g_sent.clear();
  g_incoming.clear();
  g_incomingIndex = 0;
  g_fakeNowMicros = 12345;
}

void pushIncoming(uint8_t type, uint8_t channel, uint8_t data1, uint8_t data2) {
  g_incoming.push_back({0, type, channel, data1, data2});
}

void pushIncomingAt(uint64_t timestampMicros, uint8_t type, uint8_t channel, uint8_t data1, uint8_t data2) {
  g_incoming.push_back({timestampMicros, type, channel, data1, data2});
}

int fakePollInput(void*, MidiEvent* outEvent) {
  if (g_incomingIndex >= g_incoming.size()) return 0;
  *outEvent = g_incoming[g_incomingIndex++];
  return 1;
}

void fakeScheduleOutput(void*, const MidiEvent* event, uint64_t) {
  g_sent.push_back({event->type, event->channel, event->data1, event->data2});
}

void fakeFlushOutputQueue(void*) {}
unsigned int fakeDroppedCount(void*) { return 42; } // fixed sentinel to verify the passthrough
uint64_t fakeNowMicros(void*) { return g_fakeNowMicros; }

const MidiBackend kFakeBackend = {
  nullptr, // userdata
  nullptr, nullptr, nullptr, nullptr, // port enumeration - unused here
  nullptr, nullptr, nullptr, nullptr, // open/close - unused here
  fakePollInput,
  fakeScheduleOutput,
  fakeFlushOutputQueue,
  fakeDroppedCount,
  fakeNowMicros,
  1, 1, 0, 0,
};

// -1 for every channel: Auto mode falls back to whatever instrument the
// caller passes to midiRouterTick.
const int8_t kNoChannelMap[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

}

TEST_SUITE("MIDI router") {

TEST_CASE("Incoming CC values are retained independently of note preview") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  MidiPreviewIntent intents[1];
  pushIncoming(0xB0, 3, 74, 96);
  CHECK(midiRouterTick(router, 0, intents, 1) == 0);
  uint8_t value = 0; uint32_t valueSerial = 0;
  CHECK(midiRouterGetCCValue(router, 3, 74, &value, &valueSerial));
  CHECK(value == 96);
  CHECK(valueSerial == 1);
  CHECK_FALSE(midiRouterGetCCValue(router, 3, 75, &value, &valueSerial));
  MidiCCIntent last; uint32_t serial = 0;
  REQUIRE(midiRouterGetLastCC(router, &last, &serial));
  CHECK(last.channel == 3);
  CHECK(last.cc == 74);
  CHECK(last.value == 96);
  CHECK(serial == 1);
  midiRouterDestroy(router);
}

TEST_CASE("Auto mode legato: releasing the current note resumes the previous one") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  midiRouterSetChannelInstrumentMap(router, kNoChannelMap);
  MidiPreviewIntent intents[16];

  pushIncoming(0x90, 0, 60, 100); // Note On C
  REQUIRE(midiRouterTick(router, 5, intents, 16) == 1);
  CHECK_FALSE(intents[0].stop);
  CHECK(intents[0].note == 48); // data1 - 12
  CHECK(intents[0].instrument == 5); // unmapped channel -> fallback

  g_incoming.clear(); g_incomingIndex = 0;
  pushIncoming(0x90, 0, 64, 100); // Note On E, while C is still held
  REQUIRE(midiRouterTick(router, 5, intents, 16) == 1);
  CHECK(intents[0].note == 52);

  g_incoming.clear(); g_incomingIndex = 0;
  pushIncoming(0x80, 0, 64, 0); // Note Off E (the current note)
  REQUIRE(midiRouterTick(router, 5, intents, 16) == 1);
  CHECK_FALSE(intents[0].stop);
  CHECK(intents[0].note == 48); // resumes C

  midiRouterDestroy(router);
}

TEST_CASE("Auto mode legato: releasing a buried (non-current) note does nothing") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  midiRouterSetChannelInstrumentMap(router, kNoChannelMap);
  MidiPreviewIntent intents[16];

  pushIncoming(0x90, 0, 60, 100); // Hold C
  midiRouterTick(router, 5, intents, 16);
  g_incoming.clear(); g_incomingIndex = 0;
  pushIncoming(0x90, 0, 64, 100); // Hold E (now current)
  midiRouterTick(router, 5, intents, 16);

  g_incoming.clear(); g_incomingIndex = 0;
  pushIncoming(0x80, 0, 60, 0); // Release C - buried under E, not current
  CHECK(midiRouterTick(router, 5, intents, 16) == 0);

  g_incoming.clear(); g_incomingIndex = 0;
  pushIncoming(0x80, 0, 64, 0); // Release E - now nothing remains held
  REQUIRE(midiRouterTick(router, 5, intents, 16) == 1);
  CHECK(intents[0].stop);

  midiRouterDestroy(router);
}

TEST_CASE("Auto mode keeps identical notes on separate MIDI channels independent") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  midiRouterSetChannelInstrumentMap(router, kNoChannelMap);
  MidiPreviewIntent intents[16];

  pushIncoming(0x90, 0, 60, 100);
  pushIncoming(0x90, 1, 60, 100);
  REQUIRE(midiRouterTick(router, 5, intents, 16) == 2);

  g_incoming.clear(); g_incomingIndex = 0;
  pushIncoming(0x80, 0, 60, 0); // Must not release channel 1's note.
  CHECK(midiRouterTick(router, 5, intents, 16) == 0);

  g_incoming.clear(); g_incomingIndex = 0;
  pushIncoming(0x80, 1, 60, 0);
  REQUIRE(midiRouterTick(router, 5, intents, 16) == 1);
  CHECK(intents[0].stop);

  midiRouterDestroy(router);
}

TEST_CASE("Auto mode applies Note Offs after its preview buffer is full") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  midiRouterSetChannelInstrumentMap(router, kNoChannelMap);
  MidiPreviewIntent intent;

  pushIncoming(0x90, 0, 60, 100);
  pushIncoming(0x90, 0, 64, 100);
  pushIncoming(0x80, 0, 64, 0); // This used to be discarded at capacity.
  REQUIRE(midiRouterTick(router, 5, &intent, 1) == 1);
  CHECK_FALSE(intent.stop);
  CHECK(intent.note == 48);

  g_incoming.clear(); g_incomingIndex = 0;
  pushIncoming(0x80, 0, 60, 0);
  REQUIRE(midiRouterTick(router, 5, &intent, 1) == 1);
  CHECK(intent.stop);

  midiRouterDestroy(router);
}

TEST_CASE("Channel mapping overrides the fallback instrument, per channel") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  int8_t channelMap[16];
  for (int i = 0; i < 16; i++) channelMap[i] = -1;
  channelMap[2] = 7;
  midiRouterSetChannelInstrumentMap(router, channelMap);
  MidiPreviewIntent intents[16];

  pushIncoming(0x90, 2, 60, 100); // Mapped channel
  REQUIRE(midiRouterTick(router, 5, intents, 16) == 1);
  CHECK(intents[0].instrument == 7);

  g_incoming.clear(); g_incomingIndex = 0;
  pushIncoming(0x90, 3, 60, 100); // Unmapped channel
  REQUIRE(midiRouterTick(router, 5, intents, 16) == 1);
  CHECK(intents[0].instrument == 5);

  midiRouterDestroy(router);
}

TEST_CASE("A Note On while a slot is still active releases the old note first") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();

  midiRouterEmitNoteOn(router, 0, 0, 2, 60, 100, 0);
  g_sent.clear();
  midiRouterEmitNoteOn(router, 0, 0, 2, 64, 100, 0); // same slot, no explicit Off first

  REQUIRE(g_sent.size() == 2);
  CHECK(g_sent[0].type == 0x80);
  CHECK(g_sent[0].data1 == 60);
  CHECK(g_sent[1].type == 0x90);
  CHECK(g_sent[1].data1 == 64);

  midiRouterDestroy(router);
}

TEST_CASE("Panic sends Note Off for active notes and resets the Program/Bank cache") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();

  midiRouterEmitProgramBank(router, 1, 10, EMPTY_VALUE_8, EMPTY_VALUE_8, 0);
  REQUIRE(g_sent.size() == 1);
  CHECK(g_sent[0].type == 0xC0);
  CHECK(g_sent[0].data1 == 10);

  g_sent.clear();
  midiRouterEmitProgramBank(router, 1, 10, EMPTY_VALUE_8, EMPTY_VALUE_8, 0); // unchanged - cached, no resend
  CHECK(g_sent.empty());

  midiRouterEmitNoteOn(router, 0, 0, 1, 60, 100, 0);
  g_sent.clear();
  midiRouterPanic(router);

  int sawNoteOff = 0, ccCount = 0;
  for (const SentMidiEvent& e : g_sent) {
    if (e.type == 0x80 && e.channel == 1 && e.data1 == 60) sawNoteOff = 1;
    if (e.type == 0xB0) ccCount++;
  }
  CHECK(sawNoteOff);
  CHECK(ccCount == 32); // CC123 + CC120 on each of 16 channels

  g_sent.clear();
  midiRouterEmitProgramBank(router, 1, 10, EMPTY_VALUE_8, EMPTY_VALUE_8, 0); // resends: panic cleared the cache
  REQUIRE(g_sent.size() == 1);
  CHECK(g_sent[0].type == 0xC0);

  midiRouterDestroy(router);
}

TEST_CASE("Two independent router instances don't share active-note state") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* r1 = midiRouterCreate();
  MidiRouterState* r2 = midiRouterCreate();

  midiRouterEmitNoteOn(r1, 0, 0, 1, 60, 100, 0);
  g_sent.clear();
  midiRouterPanic(r2); // must not know anything about r1's active note

  int sawR1NoteOff = 0;
  for (const SentMidiEvent& e : g_sent) if (e.type == 0x80 && e.data1 == 60) sawR1NoteOff = 1;
  CHECK_FALSE(sawR1NoteOff);

  midiRouterDestroy(r1);
  midiRouterDestroy(r2);
}

TEST_CASE("Dropped-message count passes through to the registered backend") {
  midiRouterSetBackend(&kFakeBackend);
  CHECK(midiRouterGetDroppedCount() == 42);
}

TEST_CASE("Clock mode off ignores realtime bytes entirely") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  MidiPreviewIntent intents[16];

  pushIncoming(0xF8, 0, 0, 0);
  pushIncoming(0xFA, 0, 0, 0);
  pushIncoming(0xFC, 0, 0, 0);
  CHECK(midiRouterTick(router, 5, intents, 16) == 0);
  int running = -1; uint32_t serial = 0;
  CHECK_FALSE(midiRouterGetClockTransport(router, &running, &serial));
  CHECK(serial == 0);
  float bpm = 0; uint32_t bpmSerial = 0;
  CHECK_FALSE(midiRouterGetClockBpm(router, &bpm, &bpmSerial));

  midiRouterDestroy(router);
}

TEST_CASE("Slave mode: transport messages drive running state and serial") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  midiRouterSetClockMode(router, MidiClockMode::slave);
  MidiPreviewIntent intents[16];

  pushIncoming(0xFA, 0, 0, 0); // Start
  CHECK(midiRouterTick(router, 5, intents, 16) == 0);
  int running = -1; uint32_t serial = 0;
  REQUIRE(midiRouterGetClockTransport(router, &running, &serial));
  CHECK(running == 1);
  CHECK(serial == 1);

  g_incoming.clear(); g_incomingIndex = 0;
  pushIncoming(0xFC, 0, 0, 0); // Stop
  CHECK(midiRouterTick(router, 5, intents, 16) == 0);
  REQUIRE(midiRouterGetClockTransport(router, &running, &serial));
  CHECK(running == 0);
  CHECK(serial == 2);

  g_incoming.clear(); g_incomingIndex = 0;
  pushIncoming(0xFB, 0, 0, 0); // Continue
  CHECK(midiRouterTick(router, 5, intents, 16) == 0);
  REQUIRE(midiRouterGetClockTransport(router, &running, &serial));
  CHECK(running == 1);
  CHECK(serial == 3);

  midiRouterDestroy(router);
}

TEST_CASE("Slave mode: tempo measured from clock intervals (125 BPM)") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  midiRouterSetClockMode(router, MidiClockMode::slave);
  MidiPreviewIntent intents[16];

  // 125 BPM = 20000 us per clock (60e6 / (125*24)).
  for (int i = 0; i < 8; i++) {
    pushIncomingAt(1000000 + (uint64_t)i * 20000, 0xF8, 0, 0, 0);
    midiRouterTick(router, 5, intents, 16);
  }
  float bpm = 0; uint32_t serial = 0;
  REQUIRE(midiRouterGetClockBpm(router, &bpm, &serial));
  CHECK(bpm == doctest::Approx(125.0).epsilon(0.01));
  CHECK(serial == 1); // stable tempo: one quantized change only

  midiRouterDestroy(router);
}

TEST_CASE("Slave mode: zero timestamp falls back to the backend clock") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  midiRouterSetClockMode(router, MidiClockMode::slave);
  MidiPreviewIntent intents[16];

  // No timestamps: each tick advances the fake wall clock by one clock
  // interval (20000 us = 125 BPM).
  for (int i = 0; i < 8; i++) {
    g_fakeNowMicros = 1000000 + (uint64_t)i * 20000;
    pushIncoming(0xF8, 0, 0, 0);
    midiRouterTick(router, 5, intents, 16);
  }
  float bpm = 0; uint32_t serial = 0;
  REQUIRE(midiRouterGetClockBpm(router, &bpm, &serial));
  CHECK(bpm == doctest::Approx(125.0).epsilon(0.01));

  midiRouterDestroy(router);
}

TEST_CASE("Slave mode: out-of-range clock intervals are ignored") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  midiRouterSetClockMode(router, MidiClockMode::slave);
  MidiPreviewIntent intents[16];

  // First clock anchors; second arrives 1 ms later (below the 4 ms floor -
  // a burst/catch-up artifact) and must not produce a tempo.
  pushIncomingAt(1000000, 0xF8, 0, 0, 0);
  midiRouterTick(router, 5, intents, 16);
  pushIncomingAt(1001000, 0xF8, 0, 0, 0);
  midiRouterTick(router, 5, intents, 16);
  float bpm = 0; uint32_t serial = 0;
  CHECK_FALSE(midiRouterGetClockBpm(router, &bpm, &serial));

  // A 400 ms gap (above the 300 ms ceiling - transport pause) is skipped
  // too, but the anchor moves forward so the next clock measures normally.
  pushIncomingAt(1400000, 0xF8, 0, 0, 0);
  midiRouterTick(router, 5, intents, 16);
  CHECK_FALSE(midiRouterGetClockBpm(router, &bpm, &serial));
  pushIncomingAt(1420000, 0xF8, 0, 0, 0); // 20 ms = 125 BPM
  midiRouterTick(router, 5, intents, 16);
  REQUIRE(midiRouterGetClockBpm(router, &bpm, &serial));
  CHECK(bpm == doctest::Approx(125.0).epsilon(0.01));

  midiRouterDestroy(router);
}

TEST_CASE("Slave mode: realtime bytes don't disturb note/CC routing") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  midiRouterSetClockMode(router, MidiClockMode::slave);
  MidiPreviewIntent intents[16];

  pushIncoming(0xF8, 0, 0, 0);
  pushIncoming(0xB0, 3, 74, 96);
  pushIncoming(0xFA, 0, 0, 0);
  pushIncoming(0x90, 0, 60, 100);
  pushIncoming(0xFC, 0, 0, 0);
  REQUIRE(midiRouterTick(router, 5, intents, 16) == 1);
  CHECK(intents[0].note == 48);
  uint8_t value = 0; uint32_t valueSerial = 0;
  REQUIRE(midiRouterGetCCValue(router, 3, 74, &value, &valueSerial));
  CHECK(value == 96);
  int running = -1; uint32_t serial = 0;
  REQUIRE(midiRouterGetClockTransport(router, &running, &serial));
  CHECK(running == 0); // Start then Stop
  CHECK(serial == 2);

  midiRouterDestroy(router);
}

TEST_CASE("Switching clock mode resets measurement but not serials") {
  resetFake();
  midiRouterSetBackend(&kFakeBackend);
  MidiRouterState* router = midiRouterCreate();
  midiRouterSetClockMode(router, MidiClockMode::slave);
  MidiPreviewIntent intents[16];

  pushIncoming(0xFA, 0, 0, 0);
  pushIncomingAt(1000000, 0xF8, 0, 0, 0);
  pushIncomingAt(1020000, 0xF8, 0, 0, 0);
  midiRouterTick(router, 5, intents, 16);
  int running = -1; uint32_t serial = 0;
  REQUIRE(midiRouterGetClockTransport(router, &running, &serial));
  CHECK(serial == 1);
  float bpm = 0; uint32_t bpmSerial = 0;
  REQUIRE(midiRouterGetClockBpm(router, &bpm, &bpmSerial));

  // Switch off then back on: measurement state is fresh (no tempo, not
  // running), but the transport serial survives so the app layer doesn't
  // re-apply the pre-switch Start.
  midiRouterSetClockMode(router, MidiClockMode::off);
  midiRouterSetClockMode(router, MidiClockMode::slave);
  CHECK_FALSE(midiRouterGetClockBpm(router, &bpm, &bpmSerial));
  REQUIRE(midiRouterGetClockTransport(router, &running, &serial));
  CHECK(serial == 1); // unchanged
  CHECK(running == 0); // reset

  midiRouterDestroy(router);
}

}
