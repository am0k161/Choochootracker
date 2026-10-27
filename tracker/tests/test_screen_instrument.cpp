#include "doctest.h"
#include "chipnomad_lib.h"
#include "screens.h"
#include "screen_instrument.h"

extern "C" {
// External from screen_instrument.c
extern int cInstrument;
}

#include <cstring>
#include <cstdlib>

// Test fixture
struct ScreenInstrumentFixture {
  ChipNomadState* state;

  ScreenInstrumentFixture() {
    state = chipnomadCreate();
    projectInit(&state->project);
    chipnomadState = state;
    cInstrument = 0;
  }

  ~ScreenInstrumentFixture() {
    chipnomadDestroy(state);
    chipnomadState = nullptr;
  }
};

TEST_CASE_FIXTURE(ScreenInstrumentFixture, "instrument type change none to first popup item") {
  Instrument* inst = &state->project.instruments[cInstrument];

  // Start with InstrumentType::none
  CHECK(inst->type == InstrumentType::none);

  // Simulate user pressing right arrow to increase type (row=0, col=0)
  int handled = instrumentCommonOnEdit(0, 0, editIncrease);

  // Should have handled the edit
  CHECK(handled == 1);

  CHECK(inst->type == InstrumentType::SID);
}

TEST_CASE_FIXTURE(ScreenInstrumentFixture, "instrument type change ay1 to ay2") {
  Instrument* inst = &state->project.instruments[cInstrument];

  // Initialize as AY1
  getInstrumentFunctions(InstrumentType::AY1).init(inst);
  std::strcpy(inst->name, "TestInst");
  CHECK(inst->type == InstrumentType::AY1);

  // Simulate user pressing right arrow to increase type
  int handled = instrumentCommonOnEdit(0, 0, editIncrease);

  CHECK(handled == 1);
  CHECK(inst->type == InstrumentType::AY2);

  // Verify AY2 defaults
  CHECK(inst->tableSpeed == 1);
  CHECK(inst->transposeEnabled == 1);
  CHECK(inst->chip.ay2.oscTone.isOn == 1);

  // Name should be cleared by free
  CHECK(std::strcmp(inst->name, "") == 0);
}

TEST_CASE_FIXTURE(ScreenInstrumentFixture, "instrument type change ay2 to ay sample") {
  Instrument* inst = &state->project.instruments[cInstrument];

  // Initialize as AY2
  getInstrumentFunctions(InstrumentType::AY2).init(inst);
  CHECK(inst->type == InstrumentType::AY2);

  // Simulate user pressing right arrow
  int handled = instrumentCommonOnEdit(0, 0, editIncrease);

  CHECK(handled == 1);
  CHECK(inst->type == InstrumentType::AYSample);
  CHECK(inst->tableSpeed == 1);
  CHECK(inst->chip.aySample.sampleData == nullptr);
}

TEST_CASE_FIXTURE(ScreenInstrumentFixture, "instrument type change sample with data") {
  Instrument* inst = &state->project.instruments[cInstrument];

  // Initialize as AYSample with allocated data
  getInstrumentFunctions(InstrumentType::AYSample).init(inst);
  inst->chip.aySample.sampleData = (uint8_t*)std::malloc(1024);
  CHECK(inst->chip.aySample.sampleData != nullptr);
  std::memset(inst->chip.aySample.sampleData, 0xAA, 1024);

  // Simulate user pressing right arrow to leave the AY sample engine.
  int handled = instrumentCommonOnEdit(0, 0, editIncrease);

  CHECK(handled == 1);
  CHECK(inst->type == InstrumentType::SegaPSG);
  CHECK(inst->tableSpeed == 1);

  // The old sampleData should have been freed
  // (valgrind/asan would catch if it wasn't)
}

TEST_CASE_FIXTURE(ScreenInstrumentFixture, "instrument type change decrease") {
  Instrument* inst = &state->project.instruments[cInstrument];

  // Start with AY2
  getInstrumentFunctions(InstrumentType::AY2).init(inst);
  CHECK(inst->type == InstrumentType::AY2);

  // Simulate user pressing left arrow to decrease type
  int handled = instrumentCommonOnEdit(0, 0, editDecrease);

  CHECK(handled == 1);
  CHECK(inst->type == InstrumentType::AY1);
  CHECK(inst->chip.ay.defaultMixer == 0x01);
}

TEST_CASE_FIXTURE(ScreenInstrumentFixture, "instrument type change big step") {
  Instrument* inst = &state->project.instruments[cInstrument];

  // Start with InstrumentType::none (0)
  CHECK(inst->type == InstrumentType::none);

  // Simulate user pressing Shift+Right (big step = 1 for this field)
  int handled = instrumentCommonOnEdit(0, 0, editIncreaseBig);

  CHECK(handled == 1);
  CHECK(inst->type == InstrumentType::SID);
}

TEST_CASE_FIXTURE(ScreenInstrumentFixture, "instrument type cycling follows popup order and wraps") {
  Instrument* inst = &state->project.instruments[cInstrument];

  getInstrumentFunctions(InstrumentType::GBNoise).init(inst);
  CHECK(instrumentCommonOnEdit(0, 0, editIncrease) == 1);
  CHECK(inst->type == InstrumentType::DrumSynth);

  getInstrumentFunctions(InstrumentType::MME).init(inst);
  CHECK(instrumentCommonOnEdit(0, 0, editIncrease) == 1);
  CHECK(inst->type == InstrumentType::Midi);

  getInstrumentFunctions(InstrumentType::DX7).init(inst);
  CHECK(instrumentCommonOnEdit(0, 0, editIncrease) == 1);
  CHECK(inst->type == InstrumentType::SID);

  getInstrumentFunctions(InstrumentType::SID).init(inst);
  CHECK(instrumentCommonOnEdit(0, 0, editDecrease) == 1);
  CHECK(inst->type == InstrumentType::DX7);

  inst->type = InstrumentType::none;
  CHECK(instrumentCommonOnEdit(0, 0, editDecrease) == 1);
  CHECK(inst->type == InstrumentType::DX7);
}

TEST_CASE_FIXTURE(ScreenInstrumentFixture, "instrument other fields dont affect type") {
  Instrument* inst = &state->project.instruments[cInstrument];

  // Initialize as AY1
  getInstrumentFunctions(InstrumentType::AY1).init(inst);
  CHECK(inst->type == InstrumentType::AY1);

  // Edit transpose field (row=2, col=0)
  int handled = instrumentCommonOnEdit(0, 2, editIncrease);
  CHECK(handled == 1);

  // Type should not change
  CHECK(inst->type == InstrumentType::AY1);

  // Edit table speed (row=2, col=1)
  handled = instrumentCommonOnEdit(1, 2, editIncrease);
  CHECK(handled == 1);

  // Type should still not change
  CHECK(inst->type == InstrumentType::AY1);
}
