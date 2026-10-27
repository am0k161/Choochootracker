#include "doctest.h"

#include <import/import_fur.h>
#include <export/export_fur.h>
#include <chipnomad_lib.h>
#include <project_utils.h>
#include <four_op_patch.h>
#include <opl_patch.h>
#include <sid_patch.h>
#include <synth/simple_chip_voice.h>

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

const char* FUR_PATH = "test_fur_roundtrip.fur";

std::unique_ptr<Project> newProject() {
  auto p = std::make_unique<Project>();
  projectInitAY(p.get());
  return p;
}

// One chain of one phrase per track, placed on song row 0.
void setNote(Project* p, int track, int step, int note, int instrument, int volume) {
  int chain = track, phrase = track;
  p->song[0][track] = chain;
  p->chains[chain].rows[0].phrase = phrase;
  p->chains[chain].rows[0].transpose = 0;
  PhraseRow& r = p->phrases[phrase].rows[step];
  r.note = note;
  r.instrument = instrument;
  r.volume = volume;
}

std::vector<uint8_t> readFile(const char* path) {
  std::vector<uint8_t> out;
  FILE* f = fopen(path, "rb");
  if (!f) return out;
  uint8_t buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return out;
}

// Note events per track as "step:note" (note index with the chain transpose,
// or OFF), for the tracks that have any, sorted.
std::vector<std::vector<std::string>> timelines(Project* p) {
  std::vector<std::vector<std::string>> res;
  for (int t = 0; t < PROJECT_MAX_TRACKS; t++) {
    std::vector<std::string> ev;
    for (int row = 0; row < PROJECT_MAX_LENGTH; row++) {
      uint16_t chain = p->song[row][t];
      if (chain == EMPTY_VALUE_16) continue;
      int base = row * 1000;
      int r = 0;
      for (int s = 0; s < 16; s++) {
        const ChainRow& cr = p->chains[chain].rows[s];
        if (cr.phrase == EMPTY_VALUE_16) break;
        int tr = (int8_t)cr.transpose;
        for (int i = 0; i < 16; i++, r++) {
          const PhraseRow& pr = p->phrases[cr.phrase].rows[i];
          if (pr.note == NOTE_OFF) ev.push_back(std::to_string(base + r) + ":OFF");
          else if (pr.note != EMPTY_VALUE_8) ev.push_back(std::to_string(base + r) + ":" + std::to_string(pr.note + tr));
        }
      }
    }
    if (!ev.empty()) res.push_back(ev);
  }
  std::sort(res.begin(), res.end());
  return res;
}

}

TEST_SUITE("fur") {

TEST_CASE("notes, structure and groove survive an export and import") {
  auto p = newProject();
  p->tickRate = 50;
  p->grooves[0].speed[0] = 5;
  p->grooves[0].speed[1] = 7;
  setNote(p.get(), 0, 0, 48, 0, 127);
  setNote(p.get(), 0, 4, NOTE_OFF, EMPTY_VALUE_8, EMPTY_VALUE_16);
  setNote(p.get(), 1, 2, 50, 1, 64);
  p->chains[1].rows[0].transpose = 3;
  p->song[1][0] = 0;
  snprintf(p->title, sizeof(p->title), "Fur test");

  REQUIRE(projectExportFur(p.get(), FUR_PATH) == 0);
  auto back = newProject();
  REQUIRE(projectLoadFur(back.get(), FUR_PATH, NULL) == 0);
  remove(FUR_PATH);

  CHECK(timelines(p.get()) == timelines(back.get()));
  CHECK(strcmp(back->title, "Fur test") == 0);
  CHECK(back->tickRate == doctest::Approx(50.0f));
  CHECK(back->grooves[0].speed[0] == 5);
  CHECK(back->grooves[0].speed[1] == 7);
  CHECK(back->grooves[0].speed[2] == EMPTY_VALUE_8);
  projectFree(p.get());
  projectFree(back.get());
}

TEST_CASE("furListChannels reports the channels that play notes and the automatic choice") {
  auto p = newProject();
  setNote(p.get(), 0, 0, 48, 0, 100);
  setNote(p.get(), 0, 1, 50, 0, 100);
  setNote(p.get(), 2, 0, 52, 0, 100);
  REQUIRE(projectExportFur(p.get(), FUR_PATH) == 0);

  FurChannelInfo channels[256];
  int count = 0;
  REQUIRE(furListChannels(FUR_PATH, 0, channels, 256, &count) == 0);
  REQUIRE(count == 2);
  CHECK(channels[0].notes == 2);
  CHECK(channels[1].notes == 1);
  CHECK(channels[0].picked == 1);
  CHECK(channels[1].picked == 1);
  CHECK(channels[0].channel < channels[1].channel);
  CHECK(std::string(channels[0].name).find("AY-3-8910") != std::string::npos);

  // Importing with an explicit choice keeps only that channel.
  auto back = newProject();
  int only[1] = {channels[1].channel};
  REQUIRE(projectLoadFurChannels(back.get(), FUR_PATH, NULL, only, 1, 0) == 0);
  remove(FUR_PATH);
  CHECK(back->song[0][0] != EMPTY_VALUE_16);
  CHECK(back->song[0][1] == EMPTY_VALUE_16);
  auto tl = timelines(back.get());
  REQUIRE(tl.size() == 1);
  CHECK(tl[0].size() == 1);
  CHECK(furListChannels("does-not-exist.fur", 0, channels, 256, &count) != 0);
  projectFree(p.get());
  projectFree(back.get());
}

TEST_CASE("an empty project is refused") {
  auto p = newProject();
  CHECK(projectExportFur(p.get(), FUR_PATH) != 0);
  CHECK(strlen(projectExportFurError) > 0);
  projectFree(p.get());
}

TEST_CASE("OPL3 four-operator tracks sit on the first channel of a pair and carry the 4-op flag") {
  auto p = newProject();
  Instrument* two = &p->instruments[0];
  getInstrumentFunctions(InstrumentType::OPL3).init(two);
  Instrument* four = &p->instruments[1];
  getInstrumentFunctions(InstrumentType::OPL3).init(four);
  four->chip.opl.topology = OPLTopology::fourOperator;
  four->chip.opl.connection[1] = 1;
  four->chip.opl.operators[2] = {3, 9, 15, 4, 5, 6, 1, 0, 0, 0, 0, 0};
  REQUIRE(validOPL(InstrumentType::OPL3, four->chip.opl));
  setNote(p.get(), 0, 0, 48, 0, 100);
  setNote(p.get(), 1, 0, 50, 1, 100);
  setNote(p.get(), 2, 0, 52, 0, 100);
  setNote(p.get(), 3, 0, 53, 1, 100);

  REQUIRE(projectExportFur(p.get(), FUR_PATH) == 0);
  CHECK(std::string(projectExportFurWarnings()).find("1=OPL3 ch1 2=OPL3 ch3 3=OPL3 ch5 4=OPL3 ch7") != std::string::npos);

  // The FM feature of the 4-op instrument: 36 bytes whose fourth byte has bit 5 set.
  std::vector<uint8_t> file = readFile(FUR_PATH);
  int fourFlags = 0, twoFlags = 0;
  for (size_t i = 0; i + 8 < file.size(); i++) {
    if (file[i] == 'F' && file[i + 1] == 'M' && file[i + 3] == 0) {
      if (file[i + 2] == 36 && (file[i + 4] & 15) == 4) fourFlags += (file[i + 7] & 0x20) ? 1 : 0;
      if (file[i + 2] == 20 && (file[i + 4] & 15) == 2) twoFlags += (file[i + 7] & 0x20) ? 0 : 1;
    }
  }
  CHECK(fourFlags == 1);
  CHECK(twoFlags == 1);

  auto back = newProject();
  REQUIRE(projectLoadFur(back.get(), FUR_PATH, NULL) == 0);
  remove(FUR_PATH);
  CHECK(timelines(p.get()) == timelines(back.get()));
  // Instruments come back as OPL3 patches with the operator settings.
  bool sawFour = false;
  for (int i = 0; i < PROJECT_MAX_INSTRUMENTS; i++) {
    const Instrument& in = back->instruments[i];
    if (in.type != InstrumentType::OPL3 || in.chip.opl.topology != OPLTopology::fourOperator) continue;
    sawFour = true;
    CHECK(in.chip.opl.connection[1] == 1);
    CHECK(in.chip.opl.operators[2].multiplier == 3);
    CHECK(in.chip.opl.operators[2].level == 9);
    CHECK(in.chip.opl.operators[2].waveform == 1);
  }
  CHECK(sawFour);
  projectFree(p.get());
  projectFree(back.get());
}

TEST_CASE("Genesis FM patches keep algorithm, feedback and operators") {
  auto p = newProject();
  Instrument* fm = &p->instruments[0];
  getInstrumentFunctions(InstrumentType::GenesisFM).init(fm);
  fm->chip.fourOp.algorithm = 5;
  fm->chip.fourOp.feedback = 3;
  fm->chip.fourOp.operators[1].level = 42;
  fm->chip.fourOp.operators[1].detune = 6;
  setNote(p.get(), 0, 0, 40, 0, 100);
  REQUIRE(projectExportFur(p.get(), FUR_PATH) == 0);
  auto back = newProject();
  REQUIRE(projectLoadFur(back.get(), FUR_PATH, NULL) == 0);
  remove(FUR_PATH);
  const Instrument& in = back->instruments[0];
  REQUIRE(in.type == InstrumentType::GenesisFM);
  CHECK(in.chip.fourOp.algorithm == 5);
  CHECK(in.chip.fourOp.feedback == 3);
  CHECK(in.chip.fourOp.operators[1].level == 42);
  CHECK(in.chip.fourOp.operators[1].detune == 6);
  projectFree(p.get());
  projectFree(back.get());
}

TEST_CASE("a file that is not a Furnace module is refused") {
  FILE* f = fopen(FUR_PATH, "wb");
  REQUIRE(f);
  fputs("definitely not a module", f);
  fclose(f);
  auto p = newProject();
  CHECK(projectLoadFur(p.get(), FUR_PATH, NULL) != 0);
  remove(FUR_PATH);
  projectFree(p.get());
}

TEST_CASE("SID 1Exy effects become instrument variants and the macro rate follows the song") {
  const char* demo = "/opt/furnace-0.6pre5/demos/c64/The_Snippet.fur";
  if (readFile(demo).empty()) return;
  auto p = newProject();
  REQUIRE(projectLoadFur(p.get(), demo, "snippet_samples") == 0);
  CHECK(std::string(projectLoadFurWarnings()).find("1E") == std::string::npos);
  // The bell notes set another decay or sustain with 1Exy after selecting
  // the instrument, which gives one instrument variant per combination.
  int variants = 0, decays = 0;
  uint16_t first = 0xffff;
  int sustains = 0;
  uint16_t firstSustain = 0xffff;
  for (int i = 0; i < PROJECT_MAX_INSTRUMENTS; i++) {
    const Instrument& in = p->instruments[i];
    if (in.type != InstrumentType::SID || std::string(in.name) != "bell in agony") continue;
    variants++;
    if (first == 0xffff) first = in.chip.sid.value[sidDecay];
    if (in.chip.sid.value[sidDecay] != first) decays++;
    if (firstSustain == 0xffff) firstSustain = in.chip.sid.value[sidSustain];
    if (in.chip.sid.value[sidSustain] != firstSustain) sustains++;
    if (in.chip.sid.program.size) CHECK(in.chip.sid.program.rate == 60);
  }
  CHECK(variants > 1);
  CHECK(decays + sustains > 0);
  projectFree(p.get());
}

TEST_CASE("a sample instrument without a sample feature uses sample 0") {
  const char* demo = "/opt/furnace-0.6pre5/demos/amiga/m7 vibe.fur";
  if (readFile(demo).empty()) return;
  auto p = newProject();
  REQUIRE(projectLoadFur(p.get(), demo, "fur_test_samples") == 0);
  CHECK(p->instruments[0].type == InstrumentType::Sample);
  projectFree(p.get());
  std::filesystem::remove_all("fur_test_samples");
}

TEST_CASE("Game Boy instruments become Game Boy pulse and noise instruments that sound") {
  const char* demo = "/opt/furnace-0.6pre5/demos/gameboy/snowdin.fur";
  if (readFile(demo).empty()) return;
  auto p = newProject();
  REQUIRE(projectLoadFur(p.get(), demo, NULL) == 0);
  std::string warnings = projectLoadFurWarnings();
  CHECK_MESSAGE(warnings.find("default patch used") == std::string::npos, warnings);
  MESSAGE(warnings);
  int pulses = 0, noises = 0;
  bool bell = false, fade = false;
  for (int i = 0; i < PROJECT_MAX_INSTRUMENTS; i++) {
    const Instrument& in = p->instruments[i];
    if (in.type == InstrumentType::GBPulse) pulses++;
    else if (in.type == InstrumentType::GBNoise) noises++;
    else continue;
    const InstrumentSimpleChip& d = in.chip.simpleChip;
    std::string name = in.name;
    if (name == "Bell") {
      bell = true;
      CHECK(d.envelopeInitial == 10);
      CHECK(d.envelopePeriod == 3);
      CHECK(d.envelopeIncrease == 0);
    }
    if (name.compare(0, 7, "Fade-In") == 0) {
      fade = true;
      CHECK(d.envelopeInitial == 3);
      CHECK(d.envelopeIncrease == 1);
    }
    // every instrument must produce sound
    SimpleChipVoice v;
    v.init(48000);
    v.configure(in.type, &d, 6000.f, 1.f);
    v.noteOn();
    std::vector<float> buf(2048);
    double energy = 0;
    for (int k = 0; k < 8; k++) {
      v.render(buf.data(), buf.size());
      for (float x : buf) energy += (double)x * x;
    }
    CHECK_MESSAGE(energy > 1e-6, name);
  }
  CHECK(pulses > 0);
  CHECK(noises > 0);
  CHECK(bell);
  CHECK(fade);
  projectFree(p.get());
}

namespace {
double renderEnergy(const Instrument& in) {
  SimpleChipVoice v;
  v.init(48000);
  v.configure(in.type, &in.chip.simpleChip, 6000.f, 1.f);
  v.noteOn();
  std::vector<float> buf(2048);
  double energy = 0;
  for (int k = 0; k < 8; k++) {
    v.render(buf.data(), buf.size());
    for (float x : buf) energy += (double)x * x;
  }
  return energy;
}
}

TEST_CASE("NES instruments become Game Boy pulse and noise instruments with their macros") {
  const char* demo = "/opt/furnace-0.6pre5/demos/nes/turtle_byte.fur";
  if (readFile(demo).empty()) return;
  auto p = newProject();
  REQUIRE(projectLoadFur(p.get(), demo, NULL) == 0);
  int pulses = 0, withMacros = 0, noises = 0;
  for (int i = 0; i < PROJECT_MAX_INSTRUMENTS; i++) {
    const Instrument& in = p->instruments[i];
    if (in.type != InstrumentType::GBPulse && in.type != InstrumentType::GBNoise) continue;
    if (in.type == InstrumentType::GBPulse) pulses++; else noises++;
    if (in.chip.simpleChip.program.size) withMacros++;
    CHECK_MESSAGE(renderEnergy(in) > 1e-6, in.name);
  }
  CHECK(pulses > 0);
  CHECK(noises > 0);
  CHECK(withMacros > 0);
  std::string warnings = projectLoadFurWarnings();
  CHECK(warnings.find("default patch used") == std::string::npos);
  MESSAGE(warnings);
  projectFree(p.get());
}

TEST_CASE("SNES samples, BRR samples and waveforms become sample instruments") {
  const char* dir = "fur_test_samples_snes";
  struct Demo { const char* path; int minimum; } demos[] = {
    {"/opt/furnace-0.6pre5/demos/snes/tetristheme.fur", 3},     // waveform and sample instruments
    {"/opt/furnace-0.6pre5/demos/snes/ManbowSMW.fur", 3},       // BRR samples
  };
  for (const Demo& d : demos) {
    if (readFile(d.path).empty()) continue;
    auto p = newProject();
    REQUIRE(projectLoadFur(p.get(), d.path, dir) == 0);
    int samples = 0;
    for (int i = 0; i < PROJECT_MAX_INSTRUMENTS; i++) {
      const Instrument& in = p->instruments[i];
      if (in.type != InstrumentType::Sample) continue;
      samples++;
      std::vector<uint8_t> wav = readFile(in.chip.sample.path);
      REQUIRE(wav.size() > 64);
      long peak = 0, big = 0;
      size_t n = (wav.size() - 44) / 2;
      for (size_t k = 0; k < n; k++) {
        long v = labs((int16_t)(wav[44 + 2 * k] | (wav[45 + 2 * k] << 8)));
        if (v > peak) peak = v;
        if (v > 32000) big++;
      }
      CHECK_MESSAGE(peak > 200, in.name);
      CHECK_MESSAGE(big < (long)(n / 4), in.name);      // a wrong BRR decode saturates
    }
    CHECK_MESSAGE(samples >= d.minimum, d.path);
    MESSAGE(d.path, ": ", samples, " sample instruments, ", projectLoadFurWarnings());
    projectFree(p.get());
  }
  std::filesystem::remove_all(dir);
}

}
