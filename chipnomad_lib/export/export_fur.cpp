#include "export_fur.h"
#include "../opl_patch.h"
#include "../four_op_patch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

char projectExportFurError[48];

namespace {

const int FUR_VERSION = 157;

// Function-local so that no static constructor runs where the exporter is
// compiled in but never called.
std::vector<std::string>& warningList() {
  static std::vector<std::string> list;
  return list;
}

void warn(const std::string& message) {
  std::vector<std::string>& warnings = warningList();
  for (const auto& w : warnings) if (w == message) return;
  warnings.push_back(message);
}

int fail(const char* message) {
  snprintf(projectExportFurError, sizeof(projectExportFurError), "%s", message);
  return 1;
}

// --- binary helpers ----------------------------------------------------------

typedef std::string Bytes;

Bytes u8(int v) { return Bytes(1, (char)(v & 255)); }
Bytes u16(int v) { Bytes s; s.push_back((char)(v & 255)); s.push_back((char)((v >> 8) & 255)); return s; }
Bytes u32(uint32_t v) { Bytes s; for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 255)); return s; }
Bytes f32(float v) { uint32_t x; memcpy(&x, &v, 4); return u32(x); }
Bytes str(const std::string& s) { return s + Bytes(1, '\0'); }
Bytes block(const char* id, const Bytes& body) { return Bytes(id, 4) + u32((uint32_t)body.size()) + body; }
Bytes feature(const char* code, const Bytes& data) { return Bytes(code, 2) + u16((int)data.size()) + data; }

// --- instruments -------------------------------------------------------------

struct Op {
  int mul = 1, dt = 0, tl = 0, ar = 0, rs = 0, dr = 0, am = 0, d2r = 0, rr = 0, sl = 0, ssg = 0;
  int ksr = 0, sus = 0, vib = 0, ksl = 0, ws = 0, dt2 = 0;
};

struct Patch {
  int alg = 0, fb = 0, fms = 0, ams = 0, mask = 15;
  bool four = false;
  Op ops[4];
};

enum Kind { kFm, kOpm, kOpl2, kOpl3, kSid, kAy, kKindCount, kNone = -1 };

struct KindInfo {
  const char* name;
  int chipId, perChip, furnaceType, volMax;
};

const KindInfo kinds[kKindCount] = {
  {"FM", 131, 6, 1, 127},
  {"OPM", 130, 8, 33, 127},
  {"OPL2", 144, 9, 14, 63},
  {"OPL3", 145, 18, 14, 63},
  {"SID", 7, 3, 3, 15},
  {"AY", 128, 3, 6, 15},
};

Kind kindOfType(InstrumentType type) {
  switch (type) {
    case InstrumentType::GenesisFM: return kFm;
    case InstrumentType::ArcadeFM: return kOpm;
    case InstrumentType::OPL2: return kOpl2;
    case InstrumentType::OPL3: return kOpl3;
    case InstrumentType::SID: return kSid;
    case InstrumentType::AY2: return kAy;
    case InstrumentType::SegaPSG: return kAy;
    default: return kNone;
  }
}

const int dtTable[8] = {7, 6, 5, 0, 1, 2, 3, 4};

Op defaultOp() {
  Op o;
  o.mul = 1; o.dt = 3; o.tl = 30; o.ar = 31; o.rs = 1; o.dr = 10; o.d2r = 3; o.rr = 7; o.sl = 5;
  return o;
}

Patch readFourOp(const Instrument* inst, bool arcade) {
  const InstrumentFourOp& p = inst->chip.fourOp;
  Patch fm;
  for (int i = 0; i < 4; i++) {
    const FourOpOperator& o = p.operators[i];
    int dt = 3;
    for (int k = 0; k < 8; k++) if (dtTable[k] == o.detune) { dt = k; break; }
    Op& d = fm.ops[i];
    d = Op();
    d.mul = o.multiplier; d.dt = dt; d.tl = o.level; d.rs = o.keyScale; d.ar = o.attack; d.dr = o.decay;
    d.d2r = o.sustainRate; d.rr = o.release; d.sl = o.sustainLevel;
    d.ssg = arcade ? 0 : o.ssg;
    d.dt2 = arcade ? o.detune2 : 0;
    d.am = o.amplitudeMod;
  }
  fm.alg = p.algorithm; fm.fb = p.feedback; fm.ams = p.amplitudeSensitivity; fm.fms = p.pitchSensitivity;
  fm.mask = p.operatorMask;
  return fm;
}

Patch readOpl(const Instrument* inst) {
  const InstrumentOPL& p = inst->chip.opl;
  Patch fm;
  fm.four = p.topology != OPLTopology::twoOperator;
  for (int i = 0; i < 4; i++) {
    const OPLOperator& o = p.operators[i];
    Op& d = fm.ops[i];
    d = Op();
    d.mul = o.multiplier; d.tl = o.level; d.ar = o.attack; d.dr = o.decay; d.sl = o.sustain; d.rr = o.release;
    d.ws = o.waveform; d.ksl = o.keyScale; d.vib = o.vibrato; d.am = o.tremolo; d.sus = o.sustained; d.ksr = o.rateScale;
  }
  fm.alg = fm.four ? (p.connection[0] | (p.connection[1] << 1)) : p.connection[0];
  fm.fb = p.feedback[0];
  fm.mask = fm.four ? 15 : 3;
  return fm;
}

// Feature record FM: ops are written in Furnace order (1,3,2,4 for four
// operators); `count` is the number of operators written.
Bytes fmFeature(const Patch& fm, int count) {
  static const int order4[4] = {0, 2, 1, 3};
  static const int order2[2] = {0, 1};
  const int* order = count == 4 ? order4 : order2;
  int enabled = 0;
  for (int k = 0; k < count; k++) if (fm.mask & (1 << order[k])) enabled |= 1 << k;
  Bytes data = u8((enabled << 4) | count);
  data += u8((fm.alg << 4) | fm.fb);
  data += u8((fm.ams << 3) | fm.fms);
  // bit 5 of the fourth byte is Furnace's "4 operators" flag: without it an
  // OPL3 4-op patch loads as 2-op and only its first two operators sound
  data += u8(count == 4 ? 0x20 : 0);
  for (int k = 0; k < count; k++) {
    const Op& o = fm.ops[order[k]];
    data += u8((o.ksr << 7) | (o.dt << 4) | (o.mul & 15));
    data += u8((o.sus << 7) | (o.tl & 127));
    data += u8((o.rs << 6) | (o.vib << 5) | (o.ar & 31));
    data += u8((o.am << 7) | (o.ksl << 5) | (o.dr & 31));
    data += u8(o.d2r & 31);
    data += u8((o.sl << 4) | (o.rr & 15));
    data += u8(o.ssg & 15);
    data += u8((o.dt2 << 3) | (o.ws & 7));
  }
  return feature("FM", data);
}

Bytes sidFeature(const Instrument* inst) {
  int v[12] = {0, 4, 2, 6, 10, 5, 2048, 0, 0, 0, 0, 0};
  if (inst) for (int i = 0; i < 12; i++) v[i] = inst->chip.sid.value[i];
  int wave = v[1], atk = v[2], dec = v[3], sus = v[4], rel = v[5], duty = v[6], cutoff = v[7], res = v[8];
  int filter = v[9], ring = v[10], sync = v[11];
  int toFilter = (filter != 0 || cutoff > 0) ? 1 : 0;
  int f1 = (wave & 15) | (toFilter << 4);
  int f2 = (filter & 7) | (ring << 6) | (sync << 7);
  Bytes data = u8(f1) + u8(f2) + u8((atk << 4) | dec) + u8((sus << 4) | rel);
  data += u16(duty) + u16((cutoff & 2047) | (res << 12));
  return feature("64", data);
}

Bytes ins2(int type, const std::string& name, const std::vector<Bytes>& features) {
  Bytes body = u16(FUR_VERSION) + u16(type);
  body += feature("NA", str(name));
  for (const auto& f : features) body += f;
  body += "EN";
  return block("INS2", body);
}

// Furnace INS2 block for a tracker instrument (NULL when the slot is empty)
// on a track of the given kind.
Bytes instrumentBlock(Kind kind, const Instrument* inst, int slot) {
  char defName[16];
  snprintf(defName, sizeof(defName), "Ins %02X", slot);
  std::string name = inst ? inst->name : defName;
  Kind have = inst ? kindOfType(inst->type) : kNone;
  if (inst && have != kind) {
    char buf[96];
    snprintf(buf, sizeof(buf), "instrument %02X (type %d) does not fit a %s track, default %s patch used",
             slot, (int)inst->type, kinds[kind].name, kinds[kind].name);
    warn(buf);
    inst = NULL;
  }
  int ftype = kinds[kind].furnaceType;
  switch (kind) {
    case kFm:
    case kOpm: {
      Patch fm;
      if (inst) fm = readFourOp(inst, kind == kOpm);
      else {
        fm.alg = 4;
        fm.mask = 15;
        for (int i = 0; i < 4; i++) fm.ops[i] = defaultOp();
      }
      return ins2(ftype, name, {fmFeature(fm, 4)});
    }
    case kOpl2:
    case kOpl3: {
      Patch fm;
      if (inst) {
        fm = readOpl(inst);
      } else {
        fm.mask = 3;
        fm.four = false;
        for (int i = 0; i < 4; i++) {
          Op o = defaultOp();
          o.tl = 20; o.ar = 15; o.dr = 4; o.rr = 7; o.sl = 4;
          fm.ops[i] = o;
        }
      }
      return ins2(ftype, name, {fmFeature(fm, fm.four ? 4 : 2)});
    }
    case kSid:
      return ins2(ftype, name, {sidFeature(inst)});
    default:
      return ins2(ftype, name, {});
  }
}

// --- patterns ----------------------------------------------------------------

struct Cell {
  int note = -1, ins = -1, vol = -1;
  std::vector<std::pair<int, int>> fx;
  bool empty = true;
};

// Furnace effect for a tracker FX cell: false when there is no equivalent.
bool effect(const FXCell& fx, std::pair<int, int>* out) {
  int val = fx.value;
  switch (fx.command) {
    case fxARP: *out = {0, val}; return true;
    case fxPVB: *out = {4, val}; return true;
    case fxPAN: {
      int l, r;
      if (val < 128) { l = 15; r = 15 - (int)((128 - val) * 15 / 128.0 + 0.5); }
      else { r = 15; l = 15 - (int)((val - 128) * 15 / 128.0 + 0.5); }
      *out = {8, (l << 4) | r};
      return true;
    }
    case fxOFF: *out = {236, val}; return true;
    case fxDEL: *out = {237, val}; return true;
  }
  return false;
}

Bytes encodeRows(const std::vector<Cell>& rows) {
  Bytes out;
  int empty = 0;
  for (const Cell& cell : rows) {
    if (cell.empty) { empty++; continue; }
    while (empty > 0) {
      if (empty == 1) {
        out += u8(0);
        empty = 0;
      } else {
        int n = empty < 129 ? empty : 129;
        out += u8(0x80 | (n - 2));
        empty -= n;
      }
    }
    int mask = 0;
    Bytes body;
    if (cell.note >= 0) { mask |= 1; body += u8(cell.note); }
    if (cell.ins >= 0) { mask |= 2; body += u8(cell.ins); }
    if (cell.vol >= 0) { mask |= 4; body += u8(cell.vol); }
    if (!cell.fx.empty()) {
      // bit 5: an effect mask byte follows, the cmd/value pairs come last
      mask |= 0x20;
      int fxm = 0, e = 0;
      Bytes fxb;
      for (const auto& f : cell.fx) {
        fxm |= 3 << (2 * e);
        fxb += u8(f.first) + u8(f.second);
        e++;
      }
      body = u8(fxm) + body + fxb;
    }
    out += u8(mask) + body;
  }
  out += u8(255);     // trailing empty rows are implied
  return out;
}

struct Params {
  std::vector<int> chipIds;
  int nchan = 0, patLen = 0, ordLen = 0, nIns = 0, nPat = 0;
  std::vector<std::vector<int>> orders;
  std::vector<int> speeds;
  float rate = 50;
  std::string title, author;
  std::vector<int> chanFx;
};

Bytes buildInfo(const Params& P, const std::vector<uint32_t>& insPtrs, const std::vector<uint32_t>& patPtrs,
                const uint32_t dirPtrs[3]) {
  Bytes b;
  b += u8(0) + u8(P.speeds[0]) + u8(P.speeds.size() > 1 ? P.speeds[1] : P.speeds[0]) + u8(1);
  b += f32(P.rate) + u16(P.patLen) + u16(P.ordLen) + u8(4) + u8(16);
  b += u16(P.nIns) + u16(0) + u16(0) + u32(P.nPat);
  for (int i = 0; i < 32; i++) b += u8(i < (int)P.chipIds.size() ? P.chipIds[i] : 0);
  b += Bytes(32, '\x40') + Bytes(32, '\0') + Bytes(128, '\0');
  b += str(P.title) + str(P.author) + f32(440.0f);
  static const int compat1[20] = {0, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
  for (int v : compat1) b += u8(v);
  for (uint32_t p : insPtrs) b += u32(p);
  for (uint32_t p : patPtrs) b += u32(p);
  // orders: one run of ordLen bytes per channel
  for (int c = 0; c < P.nchan; c++)
    for (const auto& o : P.orders) b += u8(o[c]);
  for (int c = 0; c < P.nchan; c++) b += u8(P.chanFx[c]);
  // channel shown (1 = visible, despite the "hide status" name in the format
  // notes: 0 leaves every channel hidden in the editor), then collapsed (0)
  b += Bytes(P.nchan, '\x01') + Bytes(P.nchan, '\0');
  for (int c = 0; c < P.nchan * 2; c++) b += str("");
  b += str("exported by ChooChooTracker") + f32(1.0f);
  static const int compat2[28] = {0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0};
  for (int v : compat2) b += u8(v);
  b += u16(1) + u16(1);
  b += str("") + str("") + u8(0) + Bytes(3, '\0');
  for (int i = 0; i < 6; i++) b += str("");
  for (size_t i = 0; i < P.chipIds.size(); i++) b += f32(1.0f) + f32(0.0f) + f32(0.0f);
  b += u32(0) + u8(1);
  b += Bytes(8, '\0');
  b += u8((int)P.speeds.size());
  for (int i = 0; i < 16; i++) b += u8(i < (int)P.speeds.size() ? P.speeds[i] : 0);
  b += u8(0);
  for (int i = 0; i < 3; i++) b += u32(dirPtrs[i]);
  return block("INFO", b);
}

struct Chain {
  std::vector<std::pair<int, int>> rows;     // phrase, transpose
};

std::vector<std::pair<int, int>> chainOf(Project* p, int id) {
  std::vector<std::pair<int, int>> out;
  if (id == EMPTY_VALUE_16 || id < 0 || id >= PROJECT_MAX_CHAINS) return out;
  for (int r = 0; r < 16; r++) {
    const ChainRow& row = p->chains[id].rows[r];
    if (row.phrase == EMPTY_VALUE_16) break;
    out.push_back({row.phrase, row.transpose});
  }
  return out;
}

}  // namespace

const char* projectExportFurWarnings(void) {
  static char text[4096];
  size_t used = 0;
  text[0] = 0;
  for (const auto& w : warningList()) {
    int n = snprintf(text + used, sizeof(text) - used, "%s%s", used ? "\n" : "", w.c_str());
    if (n < 0 || (size_t)n >= sizeof(text) - used) break;
    used += n;
  }
  return text;
}

int projectExportFur(Project* project, const char* outputPath) {
  warningList().clear();
  projectExportFurError[0] = 0;
  if (!project || !outputPath) return fail("Nothing to export");

  const int nTracks = PROJECT_MAX_TRACKS;
  int songLength = 0;
  for (int r = PROJECT_MAX_LENGTH - 1; r >= 0 && !songLength; r--)
    for (int t = 0; t < nTracks; t++)
      if (project->song[r][t] != EMPTY_VALUE_16) { songLength = r + 1; break; }
  if (songLength == 0) return fail("The project has an empty song");

  // Kind of each track: the engine of the instruments it plays most. The
  // instrument stays active from one note to the next, across phrases.
  Kind trackKind[PROJECT_MAX_TRACKS];
  bool uses4[PROJECT_MAX_TRACKS] = {false};
  for (int t = 0; t < nTracks; t++) {
    std::vector<std::pair<Kind, int>> count;      // in first-use order
    InstrumentType last = InstrumentType::none;
    bool haveLast = false;
    for (int row = 0; row < songLength; row++) {
      for (const auto& pt : chainOf(project, project->song[row][t])) {
        for (int r = 0; r < 16; r++) {
          const PhraseRow& pr = project->phrases[pt.first].rows[r];
          if (pr.instrument != EMPTY_VALUE_8 && pr.instrument < PROJECT_MAX_INSTRUMENTS) {
            const Instrument& in = project->instruments[pr.instrument];
            last = in.type;
            haveLast = in.type != InstrumentType::none;
            if (in.type == InstrumentType::OPL3 && in.chip.opl.topology != OPLTopology::twoOperator) uses4[t] = true;
          }
          if (pr.note == EMPTY_VALUE_8) continue;
          Kind k = haveLast ? kindOfType(last) : kNone;
          if (k == kNone) k = kAy;
          bool found = false;
          for (auto& c : count) if (c.first == k) { c.second++; found = true; break; }
          if (!found) count.push_back({k, 1});
        }
      }
    }
    Kind best = kNone;
    int bestN = 0;
    for (const auto& c : count) if (c.second > bestN) { best = c.first; bestN = c.second; }
    trackKind[t] = best;
  }

  // Chips: tracks of a kind fill the channels of one or more chips of it.
  std::vector<int> chipIds;
  std::vector<Kind> chanKind;
  int chanOf[PROJECT_MAX_TRACKS];
  for (int t = 0; t < nTracks; t++) chanOf[t] = -1;
  bool any = false;
  for (int t = 0; t < nTracks; t++) if (trackKind[t] != kNone) any = true;
  if (!any) return fail("The project has no notes");
  static const Kind chipOrder[kKindCount] = {kFm, kOpm, kOpl2, kOpl3, kSid, kAy};
  for (Kind k : chipOrder) {
    std::vector<int> tl;
    for (int t = 0; t < nTracks; t++) if (trackKind[t] == k) tl.push_back(t);
    if (tl.empty()) continue;
    int per = kinds[k].perChip;
    if (k == kOpl3) {
      // Furnace plays a four-operator patch only on the first channel of a
      // pair (0,1) (2,3) .. (10,11); the second one stays silent. Tracks keep
      // their order: a four-operator track starts on an even channel and
      // skips the next one, a two-operator track takes the next free channel,
      // then 12-17 once the pairs are used up.
      int base = -1, cur = 12, extra = 18;
      for (int t : tl) {
        bool need4 = uses4[t];
        int at = need4 ? ((cur + 1) & ~1) : cur;
        bool ok = base >= 0 && (need4 ? at + 2 <= 12 : (at < 12 || extra < 18));
        if (!ok) {
          base = (int)chanKind.size();
          chipIds.push_back(kinds[k].chipId);
          for (int j = 0; j < per; j++) chanKind.push_back(k);
          cur = 0; extra = 12;
          at = 0;
        }
        if (!need4 && at >= 12) at = extra++;
        else if (need4) cur = at + 2;
        else cur = at + 1;
        chanOf[t] = base + at;
      }
      continue;
    }
    int nChips = ((int)tl.size() + per - 1) / per;
    int base = (int)chanKind.size();
    for (int c = 0; c < nChips; c++) {
      chipIds.push_back(kinds[k].chipId);
      for (int j = 0; j < per; j++) chanKind.push_back(k);
    }
    int j = 0;
    for (int t : tl) chanOf[t] = base + j++;
  }
  if (chipIds.size() > 32) return fail("Too many chips");
  int nchan = (int)chanKind.size();

  // Tell the user which track went where.
  {
    std::string desc;
    for (int t = 0; t < nTracks; t++) {
      if (!desc.empty()) desc += " ";
      if (trackKind[t] == kNone) desc += std::to_string(t + 1) + "=empty";
      else desc += std::to_string(t + 1) + "=" + kinds[trackKind[t]].name + " ch" + std::to_string(chanOf[t] + 1);
    }
    warn("track to chip: " + desc);
  }

  // Instruments: one Furnace instrument per (tracker slot, kind).
  std::map<std::pair<int, int>, int> insMap;
  std::vector<Bytes> insBlocks;
  auto insFor = [&](Kind kind, int slot) -> int {
    auto key = std::make_pair(slot, (int)kind);
    auto it = insMap.find(key);
    if (it != insMap.end()) return it->second;
    int idx = (int)insBlocks.size();
    if (idx > 255) return 255;
    const Instrument* inst = (slot >= 0 && slot < PROJECT_MAX_INSTRUMENTS &&
                              project->instruments[slot].type != InstrumentType::none)
                               ? &project->instruments[slot] : NULL;
    insBlocks.push_back(instrumentBlock(kind, inst, slot));
    insMap[key] = idx;
    return idx;
  };

  // Timeline: per order, per track, the 16-row cells of the chain.
  int patLen = 16;
  for (int row = 0; row < songLength; row++)
    for (int t = 0; t < nTracks; t++) {
      int n = (int)chainOf(project, project->song[row][t]).size() * 16;
      if (n > patLen) patLen = n;
    }
  int ordLen = songLength;
  if (ordLen > 256) return fail("More than 256 orders");
  int clamped = 0, unequal = 0;
  std::map<std::string, int> dropped;
  std::vector<std::map<Bytes, int>> chanPatterns(nchan);
  std::vector<std::map<int, Bytes>> chanPatData(nchan);
  std::vector<int> chanFx(nchan, 1);
  std::vector<std::vector<int>> orders;
  for (int row = 0; row < songLength; row++) {
    std::vector<int> cells(nchan, -1);
    std::vector<int> lens;
    for (int t = 0; t < nTracks; t++) {
      if (chanOf[t] < 0) continue;
      auto chain = chainOf(project, project->song[row][t]);
      int c = chanOf[t];
      Kind kind = chanKind[c];
      int vmax = kinds[kind].volMax;
      lens.push_back((int)chain.size());
      std::vector<Cell> rows;
      for (const auto& pt : chain) {
        int tr = pt.second;
        if (tr > 127) tr -= 256;
        for (int r = 0; r < 16; r++) {
          const PhraseRow& pr = project->phrases[pt.first].rows[r];
          Cell cell;
          if (pr.note == NOTE_OFF) {
            cell.note = 180;
          } else if (pr.note != EMPTY_VALUE_8) {
            int n = pr.note + 60 + tr;
            if (n < 0) { n = 0; clamped++; }
            if (n > 179) { n = 179; clamped++; }
            cell.note = n;
          }
          if (pr.instrument != EMPTY_VALUE_8) cell.ins = insFor(kind, pr.instrument);
          if (pr.volume != EMPTY_VALUE_16) {
            int v = (int)(pr.volume * (double)vmax / 127 + 0.5);
            cell.vol = v < vmax ? v : vmax;
          }
          for (int e = 0; e < 3; e++) {
            if (pr.fx[e].command == FX_ID_EMPTY) continue;
            std::pair<int, int> out;
            if (effect(pr.fx[e], &out)) cell.fx.push_back(out);
            else dropped[fxNames[pr.fx[e].command].name]++;
          }
          cell.empty = cell.note < 0 && cell.ins < 0 && cell.vol < 0 && cell.fx.empty();
          if (!cell.empty && (int)cell.fx.size() > chanFx[c]) chanFx[c] = (int)cell.fx.size();
          rows.push_back(cell);
        }
      }
      while ((int)rows.size() < patLen) rows.push_back(Cell());
      Bytes data = encodeRows(rows);
      int pi;
      auto it = chanPatterns[c].find(data);
      if (it != chanPatterns[c].end()) {
        pi = it->second;
      } else {
        pi = (int)chanPatterns[c].size();
        if (pi > 255) return fail("More than 256 patterns on a channel");
        chanPatterns[c][data] = pi;
        chanPatData[c][pi] = data;
      }
      cells[c] = pi;
    }
    {
      std::vector<int> sorted = lens;
      bool differ = false;
      for (int l : sorted) if (l != sorted[0]) differ = true;
      if (differ) unequal++;
    }
    // Unused channels of a chip point at an empty pattern, created on demand.
    for (int c = 0; c < nchan; c++) {
      if (cells[c] >= 0) continue;
      Bytes data = encodeRows(std::vector<Cell>(patLen));
      int pi;
      auto it = chanPatterns[c].find(data);
      if (it != chanPatterns[c].end()) {
        pi = it->second;
      } else {
        pi = (int)chanPatterns[c].size();
        chanPatterns[c][data] = pi;
        chanPatData[c][pi] = data;
      }
      cells[c] = pi;
    }
    orders.push_back(cells);
  }
  if (clamped) warn(std::to_string(clamped) + " notes were outside Furnace's range and were clamped");
  if (unequal) warn(std::to_string(unequal) + " song rows have chains of different lengths: shorter tracks are padded with empty rows");
  if (!dropped.empty()) {
    std::string list;
    for (const auto& d : dropped) list += (list.empty() ? "" : ", ") + d.first + " (" + std::to_string(d.second) + ")";
    warn("effects not converted (tracker effect, count): " + list);
  }

  // PATN blocks
  std::vector<Bytes> patBlocks;
  for (int c = 0; c < nchan; c++)
    for (const auto& kv : chanPatData[c])
      patBlocks.push_back(block("PATN", u8(0) + u8(c) + u16(kv.first) + str("") + kv.second));

  // Speeds from groove 0
  std::vector<int> speeds;
  for (int i = 0; i < 16; i++) if (project->grooves[0].speed[i] != EMPTY_VALUE_8) speeds.push_back(project->grooves[0].speed[i]);
  if (speeds.empty()) speeds.push_back(6);

  Params P;
  P.chipIds = chipIds;
  P.nchan = nchan;
  P.patLen = patLen;
  P.ordLen = ordLen;
  P.orders = orders;
  P.speeds = speeds;
  P.rate = project->tickRate > 0 ? project->tickRate : 50.0f;
  P.title = project->title;
  P.author = project->author;
  P.nIns = (int)insBlocks.size();
  P.nPat = (int)patBlocks.size();
  P.chanFx = chanFx;

  // ADIR blocks (instrument, wavetable, sample directories), all empty
  Bytes adir = block("ADIR", u32(0));
  uint32_t zeroDirs[3] = {0, 0, 0};
  std::vector<uint32_t> zeroIns(P.nIns, 0), zeroPat(P.nPat, 0);
  // sizes do not depend on pointer values, so a first pass measures INFO
  Bytes info = buildInfo(P, zeroIns, zeroPat, zeroDirs);
  uint32_t pos = 32 + (uint32_t)info.size();
  // like Furnace itself: the three directory blocks right after INFO
  Bytes body = adir + adir + adir;
  uint32_t dirPtrs[3] = {pos, pos + (uint32_t)adir.size(), pos + 2 * (uint32_t)adir.size()};
  std::vector<uint32_t> insPtrs, patPtrs;
  for (const auto& b : insBlocks) { insPtrs.push_back(pos + (uint32_t)body.size()); body += b; }
  for (const auto& b : patBlocks) { patPtrs.push_back(pos + (uint32_t)body.size()); body += b; }
  info = buildInfo(P, insPtrs, patPtrs, dirPtrs);

  Bytes out = Bytes("-Furnace module-") + u16(FUR_VERSION) + u16(0) + u32(32) + Bytes(8, '\0') + info + body;
  FILE* f = fopen(outputPath, "wb");
  if (!f) return fail("Cannot write the file");
  bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  if (fclose(f) != 0) ok = false;
  if (!ok) return fail("Cannot write the file");
  return 0;
}
