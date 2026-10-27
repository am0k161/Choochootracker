#include "doctest.h"
#include "chipnomad_lib.h"
#include "pitch_table_utils.h"
#include "project.h"
#include "corelib_gfx.h"
#include "screen_instrument.h"
#include "mocks/app_ui_mock.h"
#include "synth/instrument_eq.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

float filteredRms(const InstrumentEQ& eq, float hz) {
  InstrumentEQFilter filter;
  filter.init(48000.0f);
  filter.configure(eq);
  std::vector<float> audio(2 * 48000);
  for (int i = 0; i < 48000; ++i) audio[i * 2] = audio[i * 2 + 1] = 0.1f * sinf(6.2831853f * hz * i / 48000.0f);
  for (int offset = 0; offset < 48000; offset += 480) filter.process(audio.data() + offset * 2, 480);
  double sum = 0.0;
  for (int i = 24000; i < 48000; ++i) sum += audio[i * 2] * audio[i * 2];
  return (float)sqrt(sum / 24000.0);
}

float db(float ratio) { return 20.0f * log10f(ratio); }

InstrumentEQ bell(int8_t gain) {
  InstrumentEQ eq;
  instrumentEQDefaults(&eq);
  eq.bands[1].gain = gain;
  return eq;
}

// Bank 5 holds eq; the Braids instrument uses it unless bank is EMPTY_VALUE_8.
double previewEnergy(const InstrumentEQ& eq, std::vector<float>* capture = nullptr, uint8_t bank = 5) {
  auto* s = chipnomadCreate();
  auto& p = s->project;
  p.chipsCount = 1; p.tracksCount = 1; p.tickRate = 50; p.linearPitch = 1;
  calculateLinearPitchTable12TET(&p);
  getInstrumentFunctions(InstrumentType::Braids).init(&p.instruments[0]);
  p.eqBanks[5] = eq;
  p.instruments[0].eq = bank;
  chipnomadInitChips(s, 48000, nullptr);
  REQUIRE(chipnomadQueueProjectRefresh(s));
  chipnomadQueuePlaybackPreviewNote(s, 0, 69, 0);
  double energy = 0.0;
  float audio[1920];
  for (int block = 0; block < 25; ++block) {
    chipnomadRender(s, audio, 960);
    // Braids has a small DC offset, which a high cut rightly keeps.
    for (int channel = 0; channel < 2; ++channel) {
      double mean = 0.0;
      for (int i = 0; i < 960; ++i) mean += audio[i * 2 + channel] / 960.0;
      for (int i = 0; i < 960; ++i) energy += (audio[i * 2 + channel] - mean) * (audio[i * 2 + channel] - mean);
    }
    if (capture) capture->insert(capture->end(), audio, audio + 1920);
  }
  chipnomadDestroy(s);
  return energy;
}

}  // namespace

TEST_CASE("Instruments start without an EQ, and EQ banks start flat") {
  Instrument instrument;
  getInstrumentFunctions(InstrumentType::Braids).init(&instrument);
  CHECK(instrument.eq == EMPTY_VALUE_8);
  auto project = std::make_unique<Project>();
  projectInit(project.get());
  for (const auto& eq : project->eqBanks) {
    CHECK(instrumentEQIsDefault(eq));
    CHECK_FALSE(instrumentEQAudible(eq));  // every band is at 0 dB
  }
  InstrumentEQ eq = project->eqBanks[0];
  CHECK(eq.bands[0].shape == EQShape::lowShelf);
  CHECK(eq.bands[1].shape == EQShape::bell);
  CHECK(eq.bands[2].shape == EQShape::highShelf);
  CHECK(instrumentEQQ(42) == doctest::Approx(0.7071f).epsilon(0.001));
  CHECK(instrumentEQQ(48) == doctest::Approx(1.0f));
  eq.bands[0].shape = EQShape::lowCut;
  CHECK(instrumentEQAudible(eq));  // a cut works at any gain
}

TEST_CASE("An unused EQ bank is one no instrument uses, flat ones first") {
  auto project = std::make_unique<Project>();
  projectInit(project.get());
  Project& p = *project;
  CHECK(instrumentEQUnusedBank(&p, -1) == 0);
  getInstrumentFunctions(InstrumentType::Braids).init(&p.instruments[0]);
  getInstrumentFunctions(InstrumentType::Braids).init(&p.instruments[1]);
  p.instruments[0].eq = 0;
  p.eqBanks[1].bands[1].gain = 6;  // changed but unused
  CHECK(instrumentEQUnusedBank(&p, -1) == 2);
  CHECK(instrumentEQUnusedBank(&p, 0) == 0);  // instrument 0's own bank counts as free for it
  p.instruments[1].eq = 0;                     // sharing bank 0
  CHECK(instrumentEQUnusedBank(&p, 0) == 2);
}

TEST_CASE("Instrument EQ response matches the filter") {
  const InstrumentEQ boost = bell(12);  // +6 dB at 1 kHz
  CHECK(instrumentEQResponseDb(boost, 1000.0f, 48000.0f) == doctest::Approx(6.0f).epsilon(0.01));
  CHECK(fabsf(instrumentEQResponseDb(boost, 50.0f, 48000.0f)) < 0.2f);
  CHECK(db(filteredRms(boost, 1000.0f) / filteredRms(InstrumentEQ{}, 1000.0f)) == doctest::Approx(6.0f).epsilon(0.02));

  InstrumentEQ cut;
  instrumentEQDefaults(&cut);
  cut.bands[0] = {EQShape::lowCut, 200, 0, 42};
  CHECK(instrumentEQResponseDb(cut, 200.0f, 48000.0f) == doctest::Approx(-3.0f).epsilon(0.02));
  CHECK(instrumentEQResponseDb(cut, 50.0f, 48000.0f) < -20.0f);
  CHECK(db(filteredRms(cut, 50.0f) / 0.0707f) == doctest::Approx(instrumentEQResponseDb(cut, 50.0f, 48000.0f)).epsilon(0.05));

  InstrumentEQ shelf;
  instrumentEQDefaults(&shelf);
  shelf.bands[2].gain = -12;  // -6 dB above 8 kHz, even with the EQ switched off
  CHECK(instrumentEQResponseDb(shelf, 16000.0f, 48000.0f) == doctest::Approx(-6.0f).epsilon(0.05));
  CHECK(fabsf(instrumentEQResponseDb(shelf, 200.0f, 48000.0f)) < 0.1f);
}

TEST_CASE("EQ banks and instrument EQ numbers are saved only when used or changed") {
  auto a = std::make_unique<Project>(), b = std::make_unique<Project>();
  fillFXNames();
  projectInit(a.get());
  projectInit(b.get());
  REQUIRE(projectLoad(a.get(), "packaging/common/projects/gm-midi-demo.cct") == 0);
  getInstrumentFunctions(InstrumentType::Braids).init(&a->instruments[3]);
  getInstrumentFunctions(InstrumentType::Braids).init(&a->instruments[4]);
  getInstrumentFunctions(InstrumentType::Braids).init(&a->instruments[6]);
  a->instruments[3].eq = 0x12;
  a->instruments[6].eq = 0x12;  // sharing a bank
  InstrumentEQ& eq = a->eqBanks[0x12];
  eq.bands[0] = {EQShape::lowCut, 85, 0, 60};
  eq.bands[1].gain = -17;
  eq.bands[2] = {EQShape::highCut, 12500, 4, 30};
  REQUIRE(projectSave(a.get(), "test_instrument_eq.cct") == 0);
  REQUIRE(projectLoad(b.get(), "test_instrument_eq.cct") == 0);
  CHECK(b->instruments[3].eq == 0x12);
  CHECK(b->instruments[6].eq == 0x12);
  CHECK(b->instruments[4].eq == EMPTY_VALUE_8);
  CHECK(instrumentEQSame(b->eqBanks[0x12], eq));
  for (int bank = 0; bank < PROJECT_EQ_BANKS; ++bank)
    if (bank != 0x12) CHECK(instrumentEQIsDefault(b->eqBanks[bank]));
  FILE* file = fopen("test_instrument_eq.cct", "rb");
  REQUIRE(file);
  std::string text;
  char buffer[4096];
  for (size_t n; (n = fread(buffer, 1, sizeof(buffer), file)) > 0;) text.append(buffer, n);
  fclose(file);
  // Two "- EQ: 18" instrument lines, and one changed bank.
  size_t lines = 0;
  for (size_t at = 0; (at = text.find("- EQ: ", at)) != std::string::npos; ++at) ++lines;
  CHECK(lines == 2);
  CHECK(text.find("## EQ banks\n\n12 1,85,0,60,3,1000,-17,48,5,12500,4,30\n") != std::string::npos);
  std::remove("test_instrument_eq.cct");
  projectFree(a.get());
  projectFree(b.get());
}

TEST_CASE("A saved EQ with a Q beyond the range loads with the narrowest Q") {
  auto a = std::make_unique<Project>(), b = std::make_unique<Project>();
  fillFXNames();
  projectInit(a.get());
  projectInit(b.get());
  REQUIRE(projectLoad(a.get(), "packaging/common/projects/gm-midi-demo.cct") == 0);
  a->eqBanks[0x05].bands[1] = {EQShape::bell, 1000, -12, 60};
  REQUIRE(projectSave(a.get(), "test_instrument_eq_q.cct") == 0);
  // An earlier build allowed Q codes up to 120 (Q 64).
  std::string text;
  {
    FILE* file = fopen("test_instrument_eq_q.cct", "rb");
    REQUIRE(file);
    char buffer[4096];
    for (size_t n; (n = fread(buffer, 1, sizeof(buffer), file)) > 0;) text.append(buffer, n);
    fclose(file);
  }
  const std::string saved = "05 2,100,0,42,3,1000,-12,60,";
  const size_t at = text.find(saved);
  REQUIRE(at != std::string::npos);
  text.replace(at, saved.size(), "05 2,100,0,42,3,1000,-12,110,");
  {
    FILE* file = fopen("test_instrument_eq_q.cct", "wb");
    REQUIRE(file);
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
  }
  REQUIRE(projectLoad(b.get(), "test_instrument_eq_q.cct") == 0);
  CHECK(b->eqBanks[0x05].bands[1].gain == -12);
  CHECK(b->eqBanks[0x05].bands[1].q == kEQQMax);
  CHECK(instrumentEQQ(kEQQMax) == doctest::Approx(16.0f));
  std::remove("test_instrument_eq_q.cct");
  projectFree(a.get());
  projectFree(b.get());
}

TEST_CASE("An instrument file brings its EQ into a free bank of the project") {
  auto a = std::make_unique<Project>(), b = std::make_unique<Project>();
  fillFXNames();
  projectInit(a.get());
  projectInit(b.get());
  REQUIRE(projectLoad(a.get(), "packaging/common/projects/gm-midi-demo.cct") == 0);
  REQUIRE(projectLoad(b.get(), "packaging/common/projects/gm-midi-demo.cct") == 0);
  getInstrumentFunctions(InstrumentType::Braids).init(&a->instruments[2]);
  a->instruments[2].eq = 0;
  a->eqBanks[0].bands[1].gain = 10;
  REQUIRE(instrumentSave(a.get(), "test_instrument_eq.cni", 2) == 0);

  // Bank 0 of the other project is taken by one of its instruments.
  getInstrumentFunctions(InstrumentType::Braids).init(&b->instruments[0]);
  b->instruments[0].eq = 0;
  REQUIRE(instrumentLoad(b.get(), "test_instrument_eq.cni", 9) == 0);
  const uint8_t bank = b->instruments[9].eq;
  CHECK(bank == 1);
  CHECK(instrumentEQSame(b->eqBanks[bank], a->eqBanks[0]));
  CHECK(instrumentEQIsDefault(b->eqBanks[0]));
  // Loading it again finds the same EQ instead of using another bank.
  REQUIRE(instrumentLoad(b.get(), "test_instrument_eq.cni", 10) == 0);
  CHECK(b->instruments[10].eq == bank);
  std::remove("test_instrument_eq.cni");
  projectFree(a.get());
  projectFree(b.get());
}

TEST_CASE("Instrument EQ filters the track the instrument plays on") {
  InstrumentEQ flat;
  instrumentEQDefaults(&flat);
  std::vector<float> dry, withFlat;
  const double reference = previewEnergy(flat, &dry, EMPTY_VALUE_8);
  REQUIRE(reference > 0.0);

  // A flat EQ leaves the track exactly as it was.
  previewEnergy(flat, &withFlat);
  CHECK(withFlat == dry);

  // A low high cut removes nearly everything above it.
  InstrumentEQ dark = flat;
  dark.bands[2] = {EQShape::highCut, 40, 0, 42};
  CHECK(previewEnergy(dark) < reference * 0.01);

  // The same EQ does nothing when the instrument doesn't use it.
  CHECK(previewEnergy(dark, nullptr, EMPTY_VALUE_8) == doctest::Approx(reference));
}

namespace {

struct EQPage {
  ChipNomadState* previous = chipnomadState;
  const AppScreen* previousScreen = currentScreen;
  AppSettings settings = appSettings;
  int previousInstrument = cInstrument;
  int previousModified = projectModified;
  explicit EQPage(InstrumentType type) {
    chipnomadState = chipnomadCreate();
    initDefaultAppSettings();
    screensInitAll();
    fillFXNames();
    cInstrument = 0;
    getInstrumentFunctions(type).init(&instrument());
  }
  ~EQPage() {
    chipnomadDestroy(chipnomadState);
    chipnomadState = previous;
    currentScreen = previousScreen;
    appSettings = settings;
    cInstrument = previousInstrument;
    projectModified = previousModified;
  }
  Instrument& instrument() { return chipnomadState->project.instruments[0]; }
};

}  // namespace

TEST_CASE("EQ screen edits move the band and OPT returns to the instrument") {
  EQPage e(InstrumentType::Sample);
  e.instrument().eq = 7;
  currentScreen = &screenInstrumentEQ;
  screenInstrumentEQ.setup(7);
  screenInstrumentEQ.fullRedraw();
  ScreenData* eqPage = mockScreenData;
  REQUIRE(eqPage);
  InstrumentEQBand& low = chipnomadState->project.eqBanks[7].bands[0];
  // Band row: sideways is frequency, up and down is gain.
  eqPage->onEdit(0, 0, CellEditAction::increaseBig);
  CHECK(low.gain == 2);
  eqPage->onEdit(0, 0, CellEditAction::increase);
  eqPage->onEdit(0, 0, CellEditAction::decrease);
  CHECK(low.frequency == 100);
  eqPage->onEdit(0, 2, CellEditAction::increaseBig);  // Freq: half an octave
  CHECK(low.frequency == 141);
  eqPage->onEdit(0, 1, CellEditAction::decrease);     // Shape: Lo shelf to Lo cut
  CHECK(low.shape == EQShape::lowCut);
  CHECK(eqPage->onEdit(0, 3, CellEditAction::increase) == 0);  // cuts have no gain
  eqPage->onEdit(0, 0, CellEditAction::increaseBig);  // a cut's dot moves its resonance
  CHECK(low.q == 44);
  eqPage->onEdit(0, 0, CellEditAction::clear);
  CHECK(low.shape == EQShape::lowShelf);
  CHECK(low.gain == 0);
  screenInstrumentEQ.onInput(1, keyOpt, 1);
  CHECK(currentScreen == &screenInstrument);
}
