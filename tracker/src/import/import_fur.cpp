#include <chipnomad_lib.h>
#include <project_utils.h>
#include "import_fur.h"
#include "import_common.h"
#include "corelib/corelib_file.h"
#include <four_op_patch.h>
#include <opl_patch.h>
#include <sid_patch.h>
#include <external_presets.h>
#include <synth/sample_voice.h>
#include <external/miniz/miniz_tinfl.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Furnace (.fur) reader and converter. The file format is described in
// Furnace's papers/format.md; the layout handled here is the one written by
// Furnace 0.5.x - 0.6.8 (INFO/SONG blocks) and the newer INF2/SNG2 blocks.

namespace {

// ---------------------------------------------------------------------------
// Warnings

// Function-local so that no static constructor runs where the importer is
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

// ---------------------------------------------------------------------------
// Binary reader. Reads past the end return 0 and set `bad`.

struct Reader {
  const uint8_t* d = NULL;
  size_t n = 0;
  size_t p = 0;
  bool bad = false;

  bool have(size_t count) {
    if (p > n || count > n - p) { bad = true; return false; }
    return true;
  }
  uint32_t u8() { if (!have(1)) return 0; return d[p++]; }
  int32_t s8() { return (int8_t)u8(); }
  uint32_t u16() { if (!have(2)) { p = n; return 0; } uint32_t v = d[p] | (d[p + 1] << 8); p += 2; return v; }
  int32_t s16() { return (int16_t)u16(); }
  uint32_t u32() {
    if (!have(4)) { p = n; return 0; }
    uint32_t v = d[p] | (d[p + 1] << 8) | ((uint32_t)d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24);
    p += 4;
    return v;
  }
  float f32() { uint32_t v = u32(); float f; memcpy(&f, &v, 4); return f; }
  void skip(size_t count) { if (have(count)) p += count; else p = n; }
  std::string str() {
    std::string s;
    while (p < n && d[p]) s.push_back((char)d[p++]);
    if (p < n) p++; else bad = true;
    return s;
  }
  std::string tag() {
    if (!have(4)) { p = n; return ""; }
    std::string s((const char*)d + p, 4);
    p += 4;
    return s;
  }
  std::vector<uint8_t> bytes(size_t count) {
    std::vector<uint8_t> v;
    if (!have(count)) { count = p <= n ? n - p : 0; }
    v.assign(d + p, d + p + count);
    p += count;
    return v;
  }
};

// ---------------------------------------------------------------------------
// Module model

struct FurOp {
  int mul = 1, dt = 3, ksr = 0, tl = 0, sus = 0, ar = 0, rs = 0, vib = 0, dr = 0, am = 0, ksl = 0;
  int d2r = 0, rr = 0, sl = 0, ssg = 0, ws = 0, dt2 = 0;
};

struct FurFM {
  int alg = 0, fb = 0, fms = 0, ams = 0, enabled = 15;
  std::vector<FurOp> ops;
};

struct FurC64 {
  int wave = 0, toFilter = 0, sync = 0, ring = 0, filter = 0;
  int attack = 0, decay = 0, sustain = 0, release = 0, duty = 0, cutoff = 0, res = 0;
};

struct FurIns {
  int type = -1;
  std::string name;
  bool hasFM = false, hasC64 = false;
  FurFM fm;
  FurC64 c64;
  int sample = -1;
  bool sampleMap = false;
  std::string blockId;          // INS2 or INST
  std::vector<uint8_t> raw;     // block body
};

struct FurSample {
  std::string name;
  uint32_t length = 0, rate = 0, c4 = 0;
  int depth = 0;
  int64_t loopStart = -1;
  std::vector<uint8_t> data;
};

struct FurSong {
  float tickRate = 60;
  int speed = 6, speed2 = 6, patLen = 64, ordLen = 0;
  std::vector<std::vector<int>> orders;     // order -> channel -> pattern
  std::vector<int> fxCols;
  std::vector<int> speedPattern;
  bool hasSpeedPattern = false;
  std::string name;
};

struct FurCell {
  int note = -1, ins = -1, vol = -1;
  struct Fx { int cmd, val; };
  std::vector<Fx> fx;
};
typedef std::map<int, FurCell> FurRows;

struct FurChip { int id, channels; };

struct FurModule {
  int version = 0;
  std::string name, author;
  std::vector<FurChip> chips;
  int channels = 0;
  std::vector<FurSong> songs;
  std::map<std::string, FurRows> patterns;   // "sub,chan,idx"
  std::vector<std::pair<std::string, std::vector<uint8_t>>> insBlocks;
  std::vector<FurSample> samples;
};

std::string patternKey(int sub, int chan, int idx) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%d,%d,%d", sub, chan, idx);
  return buf;
}

// Channels of every chip id Furnace knows (pairs: id, channels).
const int chipChannelTable[] = {
  1, 17, 3, 4, 4, 4, 5, 6, 6, 5, 7, 3, 71, 3, 128, 3, 129, 4, 130, 8, 131, 6, 132, 2, 133, 4, 134, 1,
  135, 8, 136, 3, 137, 9, 138, 1, 139, 3, 140, 8, 141, 6, 142, 16, 143, 9, 144, 9, 145, 18, 146, 28,
  147, 1, 148, 4, 149, 8, 150, 4, 151, 6, 152, 8, 153, 1, 154, 3, 155, 16, 156, 6, 157, 6, 158, 16,
  159, 6, 160, 9, 161, 5, 162, 11, 163, 11, 164, 20, 165, 14, 166, 17, 167, 11, 168, 4, 170, 4,
  171, 1, 172, 17, 173, 2, 174, 42, 175, 44, 176, 16, 177, 32, 178, 10, 179, 12, 180, 5, 181, 8,
  182, 9, 183, 19, 184, 8, 185, 3, 186, 8, 187, 8, 188, 8, 189, 11, 190, 7, 191, 4, 192, 1, 193, 10,
  194, 18, 195, 10, 196, 20, 197, 20, 198, 2, 199, 4, 200, 3, 202, 5, 203, 3, 204, 4, 205, 2,
  206, 24, 207, 16, 209, 18, 212, 4, 213, 6, 214, 16, 215, 2, 216, 16, 217, 4, 222, 19, 224, 19,
  226, 4, 227, 4, 228, 8, 229, 5, 231, 1, 240, 3, 241, 5, 245, 7, 252, 1, 253, 8,
};

// Legacy compound system ids flatten into several chips.
struct LegacySystem { int id; FurChip chips[2]; int count; };
const LegacySystem legacySystems[] = {
  {2, {{131, 6}, {3, 4}}, 2},
  {8, {{130, 8}, {155, 5}}, 2},
  {9, {{165, 13}, {0, 0}}, 1},
  {66, {{160, 9}, {3, 4}}, 2},
  {67, {{3, 4}, {137, 9}}, 2},
  {70, {{6, 5}, {157, 6}}, 2},
  {73, {{166, 16}, {0, 0}}, 1},
  {169, {{155, 5}, {0, 0}}, 1},
};

int chipChannelCount(int id) {
  for (size_t i = 0; i + 1 < sizeof(chipChannelTable) / sizeof(int); i += 2)
    if (chipChannelTable[i] == id) return chipChannelTable[i + 1];
  return -1;
}

bool expandChips(const std::vector<int>& ids, std::vector<FurChip>& out) {
  for (int id : ids) {
    if (id == 0) break;
    bool legacy = false;
    for (const auto& l : legacySystems) {
      if (l.id != id) continue;
      for (int i = 0; i < l.count; i++) out.push_back(l.chips[i]);
      legacy = true;
      break;
    }
    if (legacy) continue;
    int channels = chipChannelCount(id);
    if (channels < 0) return false;
    out.push_back({id, channels});
  }
  return true;
}

// ---------------------------------------------------------------------------
// Loading

// Pattern data of one pattern block (PATN, format 157 and newer).
void readPatn(Reader& r, FurModule& mod, size_t ptr) {
  r.p = ptr;
  r.bad = false;
  std::string id = r.tag();
  if (id == "PATR") {
    // Old fixed-size pattern block.
    uint32_t size = r.u32();
    size_t end = r.p + size;
    int chan = r.u16();
    int idx = r.u16();
    int sub = 0;
    if (mod.version >= 95) sub = r.u16(); else r.skip(2);
    r.skip(2);
    if (sub >= (int)mod.songs.size()) return;
    const FurSong& song = mod.songs[sub];
    int fxCols = chan < (int)song.fxCols.size() ? song.fxCols[chan] : 1;
    FurRows rows;
    (void)end;
    for (int row = 0; row < song.patLen && !r.bad; row++) {
      int note = r.s16(), oct = r.s16(), ins = r.s16(), vol = r.s16();
      FurCell cell;
      for (int e = 0; e < fxCols; e++) {
        int c = r.s16(), v = r.s16();
        if (c >= 0 || v >= 0) cell.fx.push_back({c < 0 ? -1 : c, v < 0 ? -1 : v});
      }
      int n = -1;
      if (note == 100) n = 180;
      else if (note == 101) n = 181;
      else if (note == 102) n = 182;
      else if (note != 0 || oct != 0) {
        if (oct > 127) oct -= 256;
        n = 60 + oct * 12 + (((note % 12) + 12) % 12) + (note == 12 ? 12 : 0);
      }
      if (n < 0 && ins < 0 && vol < 0 && cell.fx.empty()) continue;
      cell.note = n;
      cell.ins = ins < 0 ? -1 : ins;
      cell.vol = vol < 0 ? -1 : vol;
      rows[row] = cell;
    }
    mod.patterns[patternKey(sub, chan, idx)] = rows;
    return;
  }
  uint32_t size = r.u32();
  size_t end = r.p + size;
  int sub = r.u8();
  int chan = mod.version >= 240 ? r.u16() : r.u8();
  int idx = r.u16();
  if (mod.version >= 51) r.str();
  FurRows rows;
  int row = 0;
  while (r.p < end && !r.bad) {
    uint32_t m = r.u8();
    if (m == 0xff) break;
    if (m & 0x80) { row += (m & 0x7f) + 2; continue; }
    if (m == 0) { row++; continue; }
    uint32_t fxm = 0, fxm2 = 0;
    if (m & 0x20) fxm = r.u8();
    if (m & 0x40) fxm2 = r.u8();
    uint32_t mask = fxm | (fxm2 << 8);
    if (m & 0x08) mask |= 0x01;
    if (m & 0x10) mask |= 0x02;
    FurCell cell;
    if (m & 1) cell.note = r.u8();
    if (m & 2) cell.ins = r.u8();
    if (m & 4) cell.vol = r.u8();
    for (int e = 0; e < 8; e++) {
      int c = -1, v = -1;
      if (mask & (1u << (2 * e))) c = r.u8();
      if (mask & (1u << (2 * e + 1))) v = r.u8();
      if (c >= 0 || v >= 0) cell.fx.push_back({c, v});
    }
    if (cell.note == 183) r.u32();
    rows[row] = cell;
    row++;
  }
  mod.patterns[patternKey(sub, chan, idx)] = rows;
}

void readSample(Reader& r, FurModule& mod, size_t ptr) {
  r.p = ptr;
  r.bad = false;
  std::string id = r.tag();
  uint32_t size = r.u32();
  size_t end = size == 0 ? r.n : r.p + size;
  FurSample smp;
  smp.name = r.str();
  smp.length = r.u32();
  smp.rate = r.u32();
  uint32_t c4;
  if (id == "SMP2") {
    c4 = r.u32();
    smp.depth = r.u8();
    r.skip(3);
    uint32_t ls = r.u32(), le = r.u32();
    (void)le;
    smp.loopStart = ls > 0x7fffffffu ? -1 : (int64_t)ls;
    r.skip(16);
  } else {
    r.skip(4);
    smp.depth = r.u8();
    r.skip(1);
    c4 = smp.rate;
    if (mod.version >= 32) c4 = r.u16(); else r.skip(2);
    uint32_t ls = 0xffffffffu;
    if (mod.version >= 19) ls = r.u32(); else r.skip(4);
    smp.loopStart = ls > 0x7fffffffu ? -1 : (int64_t)ls;
  }
  smp.c4 = c4 > 0 ? c4 : smp.rate;
  size_t count = end > r.p ? end - r.p : 0;
  smp.data = r.bytes(count);
  mod.samples.push_back(smp);
}

// One subsong's channel tables.
void readSongBody(Reader& r, int nchan, FurSong& song, bool sng2) {
  std::vector<uint8_t> raw = r.bytes((size_t)nchan * song.ordLen);
  song.orders.assign(song.ordLen, std::vector<int>(nchan, 0));
  for (int o = 0; o < song.ordLen; o++)
    for (int c = 0; c < nchan; c++) {
      size_t at = (size_t)c * song.ordLen + o;
      song.orders[o][c] = at < raw.size() ? raw[at] : 0;
    }
  song.fxCols.clear();
  for (int c = 0; c < nchan; c++) song.fxCols.push_back((int)r.u8());
  r.skip((size_t)nchan * 2);                 // shown + collapsed status
  for (int c = 0; c < nchan; c++) r.str();    // channel names
  for (int c = 0; c < nchan; c++) r.str();    // short names
  if (sng2) r.skip((size_t)nchan * 4);
}

bool loadInfo(Reader& r, FurModule& mod) {
  int ver = mod.version;
  r.skip(1);
  FurSong song;
  song.speed = r.u8();
  song.speed2 = r.u8();
  r.skip(1);
  song.tickRate = r.f32();
  song.patLen = r.u16();
  song.ordLen = r.u16();
  r.skip(2);
  int insN = r.u16(), waveN = r.u16(), smpN = r.u16();
  uint32_t patN = r.u32();
  std::vector<int> ids;
  for (int i = 0; i < 32; i++) ids.push_back((int)r.u8());
  r.skip(64);
  r.skip(128);
  mod.name = r.str();
  mod.author = r.str();
  r.f32();
  r.skip(20);
  std::vector<uint32_t> insPtrs, smpPtrs, patPtrs;
  for (int i = 0; i < insN; i++) insPtrs.push_back(r.u32());
  r.skip((size_t)waveN * 4);
  for (int i = 0; i < smpN; i++) smpPtrs.push_back(r.u32());
  if (patN > 100000) return false;
  for (uint32_t i = 0; i < patN; i++) patPtrs.push_back(r.u32());
  if (r.bad) return false;
  if (!expandChips(ids, mod.chips)) return false;
  mod.channels = 0;
  for (const auto& c : mod.chips) mod.channels += c.channels;
  if (song.ordLen > 256 || song.patLen > 1024) return false;
  readSongBody(r, mod.channels, song, false);
  r.str();                                   // comment
  if (ver >= 59) r.f32();
  if (ver >= 70) r.skip(28);
  if (ver >= 96) r.skip(4);
  std::vector<uint32_t> extra;
  if (ver >= 95) {
    song.name = r.str();
    r.str();
    int nsub = r.u8();
    r.skip(3);
    for (int i = 0; i < nsub; i++) extra.push_back(r.u32());
  }
  if (ver >= 103) for (int i = 0; i < 6; i++) r.str();
  if (ver >= 135) {
    r.skip(12 * mod.chips.size());
    uint32_t nconn = r.u32();
    r.skip((size_t)nconn * 4);
    if (ver >= 136) r.skip(1);
  }
  if (ver >= 138) r.skip(8);
  if (ver >= 139) {
    int slen = r.u8();
    std::vector<int> spd;
    for (int i = 0; i < 16; i++) spd.push_back((int)r.u8());
    song.speedPattern.assign(spd.begin(), spd.begin() + (slen < 16 ? slen : 16));
    song.hasSpeedPattern = true;
    int ng = r.u8();
    r.skip((size_t)ng * 17);
  }
  mod.songs.push_back(song);
  for (uint32_t ptr : extra) {
    r.p = ptr;
    r.bad = false;
    r.tag();
    r.u32();
    r.skip(1);
    FurSong sg;
    sg.speed = r.u8();
    sg.speed2 = r.u8();
    r.skip(1);
    sg.tickRate = r.f32();
    sg.patLen = r.u16();
    sg.ordLen = r.u16();
    r.skip(2);
    r.skip(4);
    sg.name = r.str();
    r.str();
    if (sg.ordLen > 256 || sg.patLen > 1024 || r.bad) return false;
    readSongBody(r, mod.channels, sg, false);
    if (ver >= 139) {
      int slen = r.u8();
      std::vector<int> spd;
      for (int i = 0; i < 16; i++) spd.push_back((int)r.u8());
      sg.speedPattern.assign(spd.begin(), spd.begin() + (slen < 16 ? slen : 16));
      sg.hasSpeedPattern = true;
    }
    mod.songs.push_back(sg);
  }
  std::vector<uint32_t> all;
  for (auto v : insPtrs) all.push_back(v);
  for (auto v : patPtrs) all.push_back(v);
  for (auto v : extra) all.push_back(v);
  for (auto v : smpPtrs) all.push_back(v);
  for (uint32_t ptr : insPtrs) {
    r.p = ptr;
    r.bad = false;
    std::string bid = r.tag();
    uint32_t sz = r.u32();
    if (sz == 0) {
      // Very old modules wrote no block size: run to the next block.
      size_t next = r.n;
      uint32_t best = 0xffffffffu;
      for (uint32_t q : all) if (q > ptr && q < best) best = q;
      if (best != 0xffffffffu) next = best;
      sz = next > r.p ? (uint32_t)(next - r.p) : 0;
    }
    mod.insBlocks.push_back({bid, r.bytes(sz)});
  }
  for (uint32_t ptr : patPtrs) readPatn(r, mod, ptr);
  for (uint32_t ptr : smpPtrs) readSample(r, mod, ptr);
  return true;
}

bool loadInf2(Reader& r, FurModule& mod) {
  mod.name = r.str();
  mod.author = r.str();
  r.str();
  r.str();
  for (int i = 0; i < 4; i++) r.str();
  r.f32();
  r.skip(1);
  r.f32();
  int nchan = r.u16();
  int nchips = r.u16();
  for (int i = 0; i < nchips; i++) {
    int id = r.u16(), n = r.u16();
    r.skip(12);
    mod.chips.push_back({id, n});
  }
  mod.channels = nchan;
  uint32_t nconn = r.u32();
  r.skip((size_t)nconn * 4);
  r.skip(1);
  std::vector<uint32_t> songPtrs, insPtrs, patPtrs, smpPtrs;
  while (!r.bad) {
    int t = r.u8();
    if (t == 0) break;
    uint32_t n = r.u32();
    if (n > 100000) return false;
    std::vector<uint32_t> ptrs;
    for (uint32_t i = 0; i < n; i++) ptrs.push_back(r.u32());
    if (t == 1) songPtrs = ptrs;
    else if (t == 4) insPtrs = ptrs;
    else if (t == 6) smpPtrs = ptrs;
    else if (t == 7) patPtrs = ptrs;
  }
  if (r.bad) return false;
  for (uint32_t ptr : songPtrs) {
    r.p = ptr;
    r.bad = false;
    r.tag();
    r.u32();
    FurSong sg;
    sg.tickRate = r.f32();
    r.skip(2);
    sg.patLen = r.u16();
    sg.ordLen = r.u16();
    r.skip(2);
    r.skip(4);
    int slen = r.u8();
    std::vector<int> spd;
    for (int i = 0; i < 16; i++) spd.push_back((int)r.u16());
    sg.speedPattern.assign(spd.begin(), spd.begin() + (slen < 16 ? slen : 16));
    sg.hasSpeedPattern = true;
    sg.name = r.str();
    r.str();
    if (sg.ordLen > 256 || sg.patLen > 1024 || r.bad) return false;
    readSongBody(r, nchan, sg, true);
    mod.songs.push_back(sg);
  }
  for (uint32_t ptr : insPtrs) {
    r.p = ptr;
    r.bad = false;
    std::string bid = r.tag();
    uint32_t sz = r.u32();
    mod.insBlocks.push_back({bid, r.bytes(sz)});
  }
  for (uint32_t ptr : patPtrs) readPatn(r, mod, ptr);
  for (uint32_t ptr : smpPtrs) readSample(r, mod, ptr);
  return true;
}

bool furLoad(const std::vector<uint8_t>& file, FurModule& mod, const char** error) {
  static const char magic[] = "-Furnace module-";
  std::vector<uint8_t> data = file;
  if (data.size() < 16 || memcmp(data.data(), magic, 16) != 0) {
    size_t outLen = 0;
    void* out = tinfl_decompress_mem_to_heap(file.data(), file.size(), &outLen, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (!out) { *error = "Not a Furnace module"; return false; }
    data.assign((uint8_t*)out, (uint8_t*)out + outLen);
    free(out);
  }
  if (data.size() < 32 || memcmp(data.data(), magic, 16) != 0) { *error = "Not a Furnace module"; return false; }
  Reader r;
  r.d = data.data();
  r.n = data.size();
  r.p = 16;
  mod.version = r.u16();
  r.skip(2);
  uint32_t infoPtr = r.u32();
  if (infoPtr >= data.size()) { *error = "Corrupt Furnace module"; return false; }
  r.p = infoPtr;
  std::string id = r.tag();
  r.u32();
  bool ok;
  if (id == "INF2") ok = loadInf2(r, mod);
  else if (id == "INFO") ok = loadInfo(r, mod);
  else { *error = "Unknown Furnace song info"; return false; }
  if (!ok || mod.songs.empty()) {
    *error = "Unsupported Furnace module";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Instruments

FurFM parseFM(const std::vector<uint8_t>& b, int iver) {
  FurFM fm;
  if (b.size() < 4) return fm;
  int flags = b[0];
  int opCount = flags & 15;
  fm.enabled = flags >> 4;
  int b1 = b[1], b2 = b[2];
  size_t pos = iver >= 224 ? 5 : 4;
  for (int i = 0; i < opCount && pos + 8 <= b.size(); i++, pos += 8) {
    const uint8_t* o = &b[pos];
    FurOp op;
    op.mul = o[0] & 15; op.dt = (o[0] >> 4) & 7; op.ksr = o[0] >> 7;
    op.tl = o[1] & 127; op.sus = o[1] >> 7;
    op.ar = o[2] & 31; op.rs = o[2] >> 6; op.vib = (o[2] >> 5) & 1;
    op.dr = o[3] & 31; op.am = o[3] >> 7; op.ksl = (o[3] >> 5) & 3;
    op.d2r = o[4] & 31;
    op.rr = o[5] & 15; op.sl = o[5] >> 4;
    op.ssg = o[6] & 15; op.ws = o[7] & 7; op.dt2 = (o[7] >> 3) & 3;
    fm.ops.push_back(op);
  }
  fm.alg = (b1 >> 4) & 7;
  fm.fb = b1 & 7;
  fm.fms = b2 & 7;
  fm.ams = (b2 >> 3) & 3;
  return fm;
}

FurC64 parseC64(const std::vector<uint8_t>& b) {
  FurC64 c;
  if (b.size() < 8) return c;
  int f1 = b[0], f2 = b[1], ad = b[2], sr = b[3];
  int duty = b[4] | (b[5] << 8);
  int cutres = b[6] | (b[7] << 8);
  c.wave = f1 & 15;
  c.toFilter = (f1 >> 4) & 1;
  c.sync = f2 >> 7;
  c.ring = (f2 >> 6) & 1;
  c.filter = f2 & 7;
  c.attack = ad >> 4; c.decay = ad & 15; c.sustain = sr >> 4; c.release = sr & 15;
  c.duty = duty & 4095;
  c.cutoff = cutres & 2047;
  c.res = (cutres >> 12) & 15;
  return c;
}

// Old (before 127) instrument block.
FurIns parseOldInstrument(const std::vector<uint8_t>& block) {
  FurIns res;
  res.blockId = "INST";
  res.raw = block;
  if (block.size() < 6) return res;
  int iver = block[0] | (block[1] << 8);
  int itype = block[2];
  res.type = itype;
  size_t e = 4;
  while (e < block.size() && block[e]) e++;
  res.name.assign((const char*)block.data() + 4, e > 4 ? e - 4 : 0);
  std::vector<int> b;
  for (size_t i = e + 1; i < e + 1 + 160 && i < block.size(); i++) b.push_back(block[i]);
  while (b.size() < 160) b.push_back(0);
  if (itype == 3) {
    // after FM (136 bytes) and Game Boy (4 bytes): triangle saw pulse noise,
    // attack decay sustain release, duty (2), ring, sync, toFilter...
    int tri = b[140], saw = b[141], pul = b[142], noi = b[143];
    FurC64 c;
    c.attack = b[144]; c.decay = b[145]; c.sustain = b[146]; c.release = b[147];
    int d0 = b[148], d1 = b[149];
    c.ring = b[150]; c.sync = b[151]; c.toFilter = b[152];
    int rsn = b[155], lp = b[156], bp = b[157], hp = b[158];
    c.wave = tri | (saw << 1) | (pul << 2) | (noi << 3);
    c.filter = lp | (bp << 1) | (hp << 2);
    c.duty = (d0 | (d1 << 8)) & 4095;
    c.res = rsn;
    size_t cp = e + 1 + 160;
    c.cutoff = cp + 1 < block.size() ? (block[cp] | (block[cp + 1] << 8)) & 2047 : 0;
    res.hasC64 = true;
    res.c64 = c;
    return res;
  }
  if (itype == 4) {
    size_t sp = e + 1 + 164;
    if (sp + 1 < block.size()) res.sample = block[sp] | (block[sp + 1] << 8);
    return res;
  }
  if (itype != 1 && itype != 33 && itype != 14) return res;
  FurFM fm;
  fm.alg = b[0]; fm.fb = b[1]; fm.fms = b[2]; fm.ams = b[3];
  fm.enabled = 0;
  size_t pos = 8;
  for (int i = 0; i < 4; i++, pos += 32) {
    // am ar dr mult rr sl tl dt2 rs dt d2r ssg dam dvb egt ksl sus vib ws ksr en kvs
    int v[22];
    for (int k = 0; k < 22; k++) v[k] = pos + k < b.size() ? b[pos + k] : 0;
    FurOp op;
    op.am = v[0]; op.ar = v[1]; op.dr = v[2]; op.mul = v[3]; op.rr = v[4]; op.sl = v[5]; op.tl = v[6];
    op.dt2 = v[7]; op.rs = v[8]; op.dt = v[9]; op.d2r = v[10]; op.ssg = v[11] & 15;
    op.ksl = v[15]; op.sus = v[16]; op.vib = v[17]; op.ws = v[18]; op.ksr = v[19];
    int en = v[20];
    fm.ops.push_back(op);
    if (iver < 114 || en) fm.enabled |= 1 << i;
  }
  res.hasFM = true;
  res.fm = fm;
  return res;
}

// INS2 (127 and newer) or old INST block.
FurIns parseInstrument(const std::pair<std::string, std::vector<uint8_t>>& entry) {
  if (entry.first == "INST") return parseOldInstrument(entry.second);
  const std::vector<uint8_t>& block = entry.second;
  FurIns res;
  res.blockId = "INS2";
  res.raw = block;
  if (block.size() < 4) return res;
  int iver = block[0] | (block[1] << 8);
  res.type = block[2] | (block[3] << 8);
  size_t pos = 4;
  while (pos + 4 <= block.size()) {
    std::string code((const char*)block.data() + pos, 2);
    size_t flen = block[pos + 2] | (block[pos + 3] << 8);
    if (pos + 4 + flen > block.size()) flen = block.size() - pos - 4;
    std::vector<uint8_t> body(block.begin() + pos + 4, block.begin() + pos + 4 + flen);
    pos += 4 + flen;
    if (code == "EN") break;
    if (code == "NA") {
      size_t e = 0;
      while (e < body.size() && body[e]) e++;
      res.name.assign((const char*)body.data(), e);
    } else if (code == "FM") {
      res.hasFM = true;
      res.fm = parseFM(body, iver);
    } else if (code == "64") {
      res.hasC64 = true;
      res.c64 = parseC64(body);
    } else if (code == "SM" && body.size() >= 4) {
      res.sample = body[0] | (body[1] << 8);
      res.sampleMap = (body[2] | (body[3] << 8)) & 1;
    }
  }
  return res;
}

// ---------------------------------------------------------------------------
// Conversion

const int dtTable[8] = {7, 6, 5, 0, 1, 2, 3, 4};

FurOp defaultOp() {
  FurOp op;
  op.mul = 1; op.dt = 3; op.tl = 30; op.ar = 31; op.rs = 1; op.dr = 10; op.d2r = 3; op.rr = 7; op.sl = 5;
  return op;
}

FurFM defaultFM() {
  FurFM fm;
  fm.alg = 4;
  fm.enabled = 15;
  fm.ops.assign(4, defaultOp());
  return fm;
}

// Furnace stores operators in register slot order 1,3,2,4; the tracker uses
// logical order 1,2,3,4.
std::vector<FurOp> opsLogical(const FurFM& fm) {
  static const int slot[4] = {0, 2, 1, 3};
  FurOp missing;
  missing.mul = 1; missing.dt = 3; missing.tl = 127; missing.ar = 31; missing.rr = 15;
  std::vector<FurOp> out;
  for (int i = 0; i < 4; i++) out.push_back(slot[i] < (int)fm.ops.size() ? fm.ops[slot[i]] : missing);
  return out;
}

int fourOpMask(const FurFM& fm) {
  static const int logical[4] = {0, 2, 1, 3};
  int mask = 0;
  for (int bit = 0; bit < 4; bit++) if (fm.enabled & (1 << bit)) mask |= 1 << logical[bit];
  return mask ? mask : 15;
}

void setName(Instrument* inst, const std::string& name) {
  char clean[PROJECT_INSTRUMENT_NAME_LENGTH + 1];
  size_t n = 0;
  for (size_t i = 0; i < name.size() && n < PROJECT_INSTRUMENT_NAME_LENGTH; i++) {
    unsigned char c = (unsigned char)name[i];
    clean[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
  }
  while (n > 0 && clean[n - 1] == ' ') n--;
  clean[n] = 0;
  snprintf(inst->name, sizeof(inst->name), "%s", clean);
}

void copyPresetName(char* dest, size_t size, const std::string& name) {
  Instrument tmp;
  memset(&tmp, 0, sizeof(tmp));
  setName(&tmp, name);
  snprintf(dest, size, "%s", tmp.name);
}

void fourOpFromFM(Instrument* inst, const FurFM& fm, bool arcade, const std::string& name) {
  InstrumentFourOp p{};
  p.schema = 1;
  p.algorithm = fm.alg;
  p.feedback = fm.fb;
  p.pan = 3;
  p.amplitudeSensitivity = fm.ams;
  p.pitchSensitivity = fm.fms;
  p.operatorMask = fourOpMask(fm);
  copyPresetName(p.presetName, sizeof(p.presetName), name);
  std::vector<FurOp> ops = opsLogical(fm);
  for (int i = 0; i < 4; i++) {
    const FurOp& o = ops[i];
    FourOpOperator& d = p.operators[i];
    d.multiplier = o.mul;
    d.detune = dtTable[o.dt & 7];
    d.level = o.tl;
    d.keyScale = o.rs;
    d.attack = o.ar;
    d.decay = o.dr;
    d.sustainRate = o.d2r;
    d.release = o.rr;
    d.sustainLevel = o.sl;
    d.ssg = arcade ? 0 : o.ssg;
    d.detune2 = arcade ? (o.dt2 & 3) : 0;
    d.amplitudeMod = o.am;
  }
  InstrumentType type = arcade ? InstrumentType::ArcadeFM : InstrumentType::GenesisFM;
  getInstrumentFunctions(type).init(inst);
  if (validFourOp(type, p)) inst->chip.fourOp = p;
  else warn("an FM patch was out of range, default FM patch used");
}

void oplFromFM(Instrument* inst, const FurFM& fm, InstrumentType type, const std::string& name) {
  InstrumentOPL p{};
  bool four = type == InstrumentType::OPL3 && fm.ops.size() >= 4;
  std::vector<FurOp> lop;
  int c0 = fm.alg & 1, c1 = 0;
  if (four) {
    lop = opsLogical(fm);
    c1 = (fm.alg >> 1) & 1;
  } else {
    FurOp zero;
    zero.mul = 0;
    zero.dt = 0;
    for (int i = 0; i < 2; i++) lop.push_back(i < (int)fm.ops.size() ? fm.ops[i] : zero);
    lop.push_back(zero);
    lop.push_back(zero);
  }
  p.schema = 1;
  p.topology = four ? OPLTopology::fourOperator : OPLTopology::twoOperator;
  p.feedback[0] = fm.fb < 7 ? fm.fb : 7;
  p.connection[0] = c0;
  p.connection[1] = c1;
  p.pan[0] = p.pan[1] = 3;
  p.drumKey = 60;
  p.keyOnDuration = 9006;
  p.keyOffDuration = 400;
  p.volumeModel = 4;
  copyPresetName(p.presetName, sizeof(p.presetName), name);
  int wmax = type == InstrumentType::OPL2 ? 3 : 7;
  for (int i = 0; i < 4; i++) {
    const FurOp& o = lop[i];
    OPLOperator& d = p.operators[i];
    d.multiplier = o.mul & 15;
    d.level = o.tl & 63;
    d.attack = o.ar < 15 ? o.ar : 15;
    d.decay = o.dr < 15 ? o.dr : 15;
    d.sustain = o.sl;
    d.release = o.rr;
    d.waveform = o.ws < wmax ? o.ws : wmax;
    d.keyScale = o.ksl & 3;
    d.vibrato = o.vib;
    d.tremolo = o.am;
    d.sustained = o.sus;
    d.rateScale = o.ksr;
  }
  getInstrumentFunctions(type).init(inst);
  if (validOPL(type, p)) inst->chip.opl = p;
  else warn("an OPL patch was out of range, default OPL patch used");
}

void sidFromC64(Instrument* inst, const FurC64* c, const std::string& name) {
  InstrumentSID p{};
  p.schema = 1;
  copyPresetName(p.presetName, sizeof(p.presetName), name);
  p.value[sidPartnerRatio] = 2;
  p.value[sidMacroRate] = 50;
  p.value[sidCutoffTarget] = 65535;
  if (!c) {
    p.value[sidWave] = 4;
    p.value[sidDecay] = 6;
    p.value[sidSustain] = 10;
    p.value[sidRelease] = 5;
    p.value[sidPulse] = 2048;
  } else {
    int w = c->wave;
    if (w & 8) w = 8; else if (w == 0) w = 4;
    p.value[sidWave] = w;
    p.value[sidAttack] = c->attack;
    p.value[sidDecay] = c->decay;
    p.value[sidSustain] = c->sustain;
    p.value[sidRelease] = c->release;
    p.value[sidPulse] = c->duty;
    if (c->toFilter) {
      p.value[sidCutoff] = c->cutoff;
      p.value[sidResonance] = c->res;
      p.value[sidFilterMode] = c->filter;
    }
    p.value[sidRing] = c->ring;
    p.value[sidSync] = c->sync;
  }
  getInstrumentFunctions(InstrumentType::SID).init(inst);
  if (validSID(p)) inst->chip.sid = p;
  else warn("a SID patch was out of range, default SID patch used");
}

// A Furnace instrument through the app's own Furnace instrument importer,
// which also converts the macros the engine has lanes for: volume, arpeggio,
// pulse width, waveform and cutoff on a SID, volume, arpeggio and noise mode on
// the Sega PSG. False (with the reason in `why`) when it cannot take the
// instrument; otherwise `why` lists what was dropped, empty when nothing was.
bool instrumentFromFurnace(Instrument* inst, InstrumentType target, const std::string& blockId,
                           const std::vector<uint8_t>& body, std::string* why) {
  if (body.size() < 4) { *why = "no instrument data"; return false; }
  std::vector<uint8_t> data;
  if (blockId == "INST") {
    // Old instruments (before format 127): rebuild the legacy .fui container.
    static const char magic[] = "-Furnace instr.-";
    data.assign(magic, magic + 16);
    data.push_back(body[0]); data.push_back(body[1]);          // version, as in the block
    data.insert(data.end(), {0, 0, 32, 0, 0, 0, 0, 0, 0, 0});  // reserved, offset 32, no waves/samples
    data.resize(32, 0);
    data.insert(data.end(), {'I', 'N', 'S', 'T'});
    for (int i = 0; i < 4; i++) data.push_back((uint32_t)body.size() >> (8 * i));
    data.insert(data.end(), body.begin(), body.end());
  } else {
    data = {'F', 'I', 'N', 'S'};
    data.insert(data.end(), body.begin(), body.end());
  }
  ExternalPreset preset;
  std::string dropped;
  if (!importFurnaceInstrument(target, data, preset, dropped)) {
    *why = dropped.empty() ? "not supported" : dropped;
    return false;
  }
  *inst = preset.instrument;
  *why = dropped;
  return true;
}

void psgInstrument(Instrument* inst, bool noise) {
  InstrumentSimpleChip p{};
  p.schema = 1;
  p.mode = noise ? 1 : 0;
  p.envelopeInitial = 15;
  p.segaBassExtension = 1;
  getInstrumentFunctions(InstrumentType::SegaPSG).init(inst);
  inst->chip.simpleChip = p;
}

void ayInstrument(Instrument* inst) {
  getInstrumentFunctions(InstrumentType::AY2).init(inst);
  inst->chip.ay2.oscTone.isOn = 1;
  inst->chip.ay2.oscNoise.isOn = 0;
  inst->chip.ay2.oscEnvelope.shape = 0;
}

struct ChannelKind {
  std::string kind;     // fm opm opl2 opl3 ay sid psg noise generic
  int chipId;
  int local;
};

std::vector<ChannelKind> channelKinds(const FurModule& mod) {
  std::vector<ChannelKind> out;
  for (const auto& chip : mod.chips) {
    for (int i = 0; i < chip.channels; i++) {
      std::string k;
      switch (chip.id) {
        case 131: case 160: k = "fm"; break;
        case 189: k = i < 9 ? "fm" : "generic"; break;
        case 190: k = i < 6 ? "fm" : "generic"; break;
        case 130: k = "opm"; break;
        case 128: case 154: k = "ay"; break;
        case 143: case 144: k = "opl2"; break;
        case 145: k = "opl3"; break;
        case 7: case 71: k = "sid"; break;
        case 3: k = i == 3 ? "noise" : "psg"; break;
        default: k = "generic";
      }
      out.push_back({k, chip.id, i});
    }
  }
  return out;
}

std::string chipName(int id) {
  static const struct { int id; const char* name; } names[] = {
    {3, "SN76489"}, {4, "Game Boy"}, {5, "PC Engine"}, {6, "NES"}, {7, "C64 8580"}, {71, "C64 6581"},
    {128, "AY-3-8910"}, {129, "Amiga"}, {130, "YM2151"}, {131, "YM2612"}, {132, "TIA"}, {133, "VIC-20"},
    {135, "SNES"}, {136, "VRC6"}, {137, "OPLL"}, {138, "FDS"}, {139, "MMC5"}, {140, "Namco 163"},
    {141, "YM2203"}, {142, "YM2608"}, {143, "OPL"}, {144, "OPL2"}, {145, "OPL3"}, {151, "SAA1099"},
    {152, "OPZ"}, {155, "SegaPCM"}, {157, "VRC7"}, {158, "YM2610B"}, {165, "YM2610"},
  };
  for (const auto& n : names) if (n.id == id) return n.name;
  char buf[24];
  snprintf(buf, sizeof(buf), "chip 0x%02X", id);
  return buf;
}

// Mono 16-bit WAV of a Furnace sample. False when the depth is unsupported.
bool sampleToWav(const FurSample& smp, std::vector<uint8_t>& wav, int* rate) {
  std::vector<int16_t> v;
  const std::vector<uint8_t>& data = smp.data;
  switch (smp.depth) {
    case 8:
      if (data.size() >= 2 * (size_t)smp.length && smp.length > 0 && data.size() % 2 == 0 && data.size() != smp.length) {
        for (size_t i = 0; i + 1 < data.size(); i += 2) v.push_back((int16_t)(data[i] | (data[i + 1] << 8)));
      } else {
        for (uint8_t x : data) v.push_back((int16_t)((int8_t)x * 256));
      }
      break;
    case 16:
      for (size_t i = 0; i + 1 < data.size(); i += 2) v.push_back((int16_t)(data[i] | (data[i + 1] << 8)));
      break;
    case 11:
      for (uint8_t b : data) {
        int x = (~b) & 255;
        int t = ((x & 15) << 3) + 132;
        t <<= (x & 0x70) >> 4;
        v.push_back((int16_t)((x & 0x80) ? 132 - t : t - 132));
      }
      break;
    default:
      return false;
  }
  if (v.empty()) return false;
  *rate = (int)smp.c4;
  uint32_t n = (uint32_t)v.size() * 2;
  wav.clear();
  auto put32 = [&](uint32_t x) { for (int i = 0; i < 4; i++) wav.push_back((x >> (8 * i)) & 255); };
  auto put16 = [&](uint32_t x) { for (int i = 0; i < 2; i++) wav.push_back((x >> (8 * i)) & 255); };
  wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
  put32(36 + n);
  wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(16); put16(1); put16(1); put32(*rate); put32(*rate * 2); put16(2); put16(16);
  wav.insert(wav.end(), {'d', 'a', 't', 'a'});
  put32(n);
  for (int16_t s : v) put16((uint16_t)s);
  return true;
}

struct Slot {
  std::string kind;
  std::string name;
  int furIns = -1;
  bool sample = false;
  std::vector<uint8_t> wav;
  int rate = 0;
  bool loop = false;
  // SID parameters set by 1Exy effects: attack, decay, sustain, release, ring, sync (-1: instrument's own).
  int sidOv[6] = {-1, -1, -1, -1, -1, -1};
};

struct Converter {
  FurModule& mod;
  std::vector<FurIns> furIns;
  std::map<std::string, int> slotOf;
  std::vector<Slot> slots;
  Project& p;

  Converter(FurModule& m, Project& project) : mod(m), p(project) {}

  void buildKind(Instrument* inst, const std::string& kind, const FurIns* ins, int fi, const std::string& nm) {
    int t = ins ? ins->type : -1;
    if (kind == "fm") {
      if (t == 1 && ins->hasFM) fourOpFromFM(inst, ins->fm, false, nm);
      else {
        if (fi >= 0) { char b[96]; snprintf(b, sizeof(b), "instrument %d (Furnace type %d) is not a YM2612 FM instrument, default FM patch used", fi, t); warn(b); }
        fourOpFromFM(inst, defaultFM(), false, nm);
      }
    } else if (kind == "opm") {
      if (t == 33 && ins->hasFM) fourOpFromFM(inst, ins->fm, true, nm);
      else {
        if (fi >= 0) { char b[96]; snprintf(b, sizeof(b), "instrument %d (Furnace type %d) is not a YM2151 instrument, default patch used", fi, t); warn(b); }
        fourOpFromFM(inst, defaultFM(), true, nm);
      }
    } else if (kind == "opl2" || kind == "opl3") {
      InstrumentType type = kind == "opl2" ? InstrumentType::OPL2 : InstrumentType::OPL3;
      if (t == 14 && ins->hasFM) oplFromFM(inst, ins->fm, type, nm);
      else {
        if (fi >= 0) { char b[96]; snprintf(b, sizeof(b), "instrument %d (Furnace type %d) is not an OPL instrument, default patch used", fi, t); warn(b); }
        oplFromFM(inst, defaultFM(), type, nm);
      }
    } else if (kind == "ay") {
      ayInstrument(inst);
    } else if (kind == "sid") {
      std::string why;
      if (t == 3 && ins->hasC64 && instrumentFromFurnace(inst, InstrumentType::SID, ins->blockId, ins->raw, &why)) {
        if (!why.empty()) {
          char b[200];
          snprintf(b, sizeof(b), "SID instrument %d: not converted: %.120s", fi, why.c_str());
          warn(b);
        }
      } else {
        if (t == 3 && ins->hasC64) {
          char b[200];
          snprintf(b, sizeof(b), "SID instrument %d: macros not converted (%.120s)", fi, why.c_str());
          warn(b);
        }
        sidFromC64(inst, (t == 3 && ins->hasC64) ? &ins->c64 : NULL, nm);
      }
    } else if ((kind == "noise" || kind == "psg") && t == 0 && !ins->raw.empty()) {
      std::string why;
      if (instrumentFromFurnace(inst, InstrumentType::SegaPSG, ins->blockId, ins->raw, &why)) {
        // The noise channel plays noise unless a duty macro picks the mode.
        if (kind == "noise") inst->chip.simpleChip.mode = 1;
        if (!why.empty()) {
          char b[200];
          snprintf(b, sizeof(b), "PSG instrument %d: not converted: %.120s", fi, why.c_str());
          warn(b);
        }
      } else {
        char b[200];
        snprintf(b, sizeof(b), "PSG instrument %d: macros not converted (%.120s)", fi, why.c_str());
        warn(b);
        psgInstrument(inst, kind == "noise");
      }
    } else if (kind == "noise") {
      psgInstrument(inst, true);
    } else {
      psgInstrument(inst, false);
    }
    setName(inst, nm);
  }

  // Slot of a Furnace instrument on a channel of the given kind.
  int slotFor(int fi, const std::string& kind, bool canSample, const int* sidOv = NULL) {
    char key[80];
    int n = snprintf(key, sizeof(key), "%d,%s", fi, kind.c_str());
    if (sidOv)
      for (int i = 0; i < 6; i++) n += snprintf(key + n, sizeof(key) - n, ",%d", sidOv[i]);
    auto it = slotOf.find(key);
    if (it != slotOf.end()) return it->second;
    if (slots.size() >= PROJECT_MAX_INSTRUMENTS) {
      warn("more than 128 instruments needed, extra ones use instrument 7F");
      return PROJECT_MAX_INSTRUMENTS - 1;
    }
    int slot = (int)slots.size();
    const FurIns* ins = (fi >= 0 && fi < (int)furIns.size()) ? &furIns[fi] : NULL;
    std::string nm = ins ? ins->name : "";
    if (nm.empty()) { char b[16]; snprintf(b, sizeof(b), "Ins %02X", fi < 0 ? 0 : fi); nm = b; }
    Slot s;
    s.kind = kind;
    s.name = nm;
    s.furIns = fi;
    if (sidOv) memcpy(s.sidOv, sidOv, sizeof(s.sidOv));
    if (ins && ins->type == 4 && ins->sample >= 0) {
      const FurSample* smp = ins->sample < (int)mod.samples.size() ? &mod.samples[ins->sample] : NULL;
      std::vector<uint8_t> wav;
      int rate = 0;
      if (smp && canSample && sampleToWav(*smp, wav, &rate)) {
        if (ins->sampleMap) { char b[80]; snprintf(b, sizeof(b), "instrument %d uses a sample map: only its initial sample is converted", fi); warn(b); }
        s.sample = true;
        s.wav = wav;
        s.rate = rate;
        s.loop = smp->loopStart >= 0;
        if (s.loop && smp->loopStart > 0) warn("sample '" + smp->name + "' loops from a later frame: the tracker loops the whole sample");
      } else if (smp && !canSample) {
        warn("Furnace samples need the sample folder: sample instruments use a default patch");
      } else {
        char b[96];
        snprintf(b, sizeof(b), "instrument %d: sample depth %d is not supported, default patch used", fi, smp ? smp->depth : 0);
        warn(b);
      }
    }
    slots.push_back(s);
    slotOf[key] = slot;
    return slot;
  }
};

// Furnace effect -> tracker FX cell, false when there is no equivalent.
bool mapEffect(int cmd, int val, std::map<int, int>& grooves, FXCell* out) {
  switch (cmd) {
    case 0:
      if (val == 0) return false;
      out->command = fxARP; out->value = val; return true;
    case 4:
      out->command = fxPVB; out->value = val; return true;
    case 8: {
      int l = val >> 4, r = val & 15;
      if (l == 0 && r == 0) return false;
      int v = (int)(128 + (r - l) * 128.0 / 15);
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      out->command = fxPAN; out->value = v; return true;
    }
    case 15: {
      if (val == 0 || val > 31) return false;
      auto it = grooves.find(val);
      if (it == grooves.end()) {
        if (grooves.size() >= 31) return false;
        it = grooves.insert({val, (int)grooves.size() + 1}).first;
      }
      out->command = fxGGR; out->value = it->second; return true;
    }
    case 236: out->command = fxOFF; out->value = val; return true;
    case 237: out->command = fxDEL; out->value = val; return true;
  }
  return false;
}

// Default channel choice: channels that have notes, at most 8. Channels
// playing sample instruments come first (they are what distinguishes a
// module), the rest in channel order.
std::vector<int> autoChannels(const FurModule& mod, int songIdx, const std::vector<ChannelKind>& chanKinds, const std::vector<FurIns>& ins) {
  const int nkinds = (int)chanKinds.size();
  std::vector<bool> notes(nkinds, false), sampled(nkinds, false);
  for (const auto& kv : mod.patterns) {
    int sub, ch, pat;
    if (sscanf(kv.first.c_str(), "%d,%d,%d", &sub, &ch, &pat) != 3) continue;
    if (sub != songIdx || ch < 0 || ch >= nkinds) continue;
    for (const auto& rc : kv.second) {
      if (rc.second.note >= 0) notes[ch] = true;
      int i = rc.second.ins;
      if (i >= 0 && i < (int)ins.size() && ins[i].type == 4) sampled[ch] = true;
    }
  }
  std::vector<int> used;
  for (int c = 0; c < nkinds; c++) if (notes[c]) used.push_back(c);
  if (used.empty()) for (int c = 0; c < nkinds; c++) used.push_back(c);
  if (used.size() <= 8) return used;
  std::vector<int> pick;
  for (int c : used) if (sampled[c] && pick.size() < 2) pick.push_back(c);
  // Channels of a chip that has instruments here come before the ones kept
  // with a default square instrument.
  for (int pass = 0; pass < 2; pass++) {
    for (int c : used) {
      bool in = false;
      for (int q : pick) in = in || q == c;
      bool generic = chanKinds[c].kind == "generic";
      if (!in && pick.size() < 8 && generic == (pass == 1)) pick.push_back(c);
    }
  }
  std::sort(pick.begin(), pick.end());
  std::string kept, dropped;
  for (int c : pick) kept += (kept.empty() ? "" : ",") + std::to_string(c);
  for (int c : used) {
    bool in = false;
    for (int q : pick) in = in || q == c;
    if (!in) dropped += (dropped.empty() ? "" : ",") + std::to_string(c);
  }
  warn("module has " + std::to_string(used.size()) + " channels with notes, keeping " + kept + " and dropping " + dropped);
  return pick;
}

bool readWholeFile(const char* path, std::vector<uint8_t>& out) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > 64 * 1024 * 1024) { fclose(f); return false; }
  out.resize(size);
  size_t got = fread(out.data(), 1, size, f);
  fclose(f);
  return got == (size_t)size;
}

std::string fileBase(const char* path) {
  const char* slash = strrchr(path, '/');
  const char* back = strrchr(path, '\\');
  if (back && (!slash || back > slash)) slash = back;
  std::string name = slash ? slash + 1 : path;
  size_t dot = name.rfind('.');
  if (dot != std::string::npos) name.resize(dot);
  std::string clean;
  for (char c : name) clean.push_back((c == '/' || c == '\\' || c == ':' || (unsigned char)c < 0x20) ? '_' : c);
  if (clean.empty()) clean = "fur";
  return clean;
}

}  // namespace

const char* projectLoadFurWarnings(void) {
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

int projectLoadFurChannels(Project* project, const char* path, const char* sampleDir,
                           const int* channels, int channelCount, int subsong) {
  warningList().clear();
  if (!project || !path) return 1;
  std::vector<uint8_t> file;
  if (!readWholeFile(path, file)) {
    snprintf(projectFileError, 40, "Cannot open Furnace file");
    return 1;
  }
  FurModule mod;
  const char* error = NULL;
  if (!furLoad(file, mod, &error)) {
    snprintf(projectFileError, 40, "%s", error ? error : "Not a Furnace module");
    return 1;
  }
  int songIdx = subsong;
  if (songIdx < 0 || songIdx >= (int)mod.songs.size()) {
    snprintf(projectFileError, 40, "No such Furnace subsong");
    return 1;
  }
  const FurSong& song = mod.songs[songIdx];

  auto owned = std::make_unique<Project>();
  Project& p = *owned;
  projectInitAY(&p);

  std::vector<ChannelKind> kinds = channelKinds(mod);
  std::vector<FurIns> furIns;
  for (const auto& blk : mod.insBlocks) furIns.push_back(parseInstrument(blk));

  std::vector<int> chans;
  if (channels && channelCount > 0) chans.assign(channels, channels + channelCount);
  else chans = autoChannels(mod, songIdx, kinds, furIns);
  if (chans.empty()) {
    snprintf(projectFileError, 40, "Module has no channels");
    return 1;
  }
  if (chans.size() > (size_t)PROJECT_MAX_TRACKS) {
    snprintf(projectFileError, 40, "At most 8 channels can be imported");
    return 1;
  }
  std::map<int, std::vector<int>> generic;
  for (int c : chans) {
    if (c < 0 || c >= (int)kinds.size()) {
      snprintf(projectFileError, 40, "Channel %d does not exist", c);
      return 1;
    }
    if (kinds[c].kind == "generic") generic[kinds[c].chipId].push_back(c);
  }
  for (const auto& g : generic) {
    std::string list;
    for (int c : g.second) list += (list.empty() ? "" : ",") + std::to_string(c);
    warn(chipName(g.first) + " is not supported yet: notes and structure of channel(s) " + list +
         " are kept with a default square instrument, instruments and effects are not converted");
  }

  Converter conv(mod, p);
  conv.furIns = furIns;
  bool canSample = sampleDir && sampleDir[0];

  // Furnace volume range per channel: known per chip, else the next power of
  // two above the largest volume the channel uses.
  std::vector<int> volMax(chans.size(), 15);
  for (size_t i = 0; i < chans.size(); i++) {
    const std::string& k = kinds[chans[i]].kind;
    if (k == "fm" || k == "opm") volMax[i] = 127;
    else if (k == "opl2" || k == "opl3") volMax[i] = 63;
    else if (k == "ay" || k == "sid" || k == "psg" || k == "noise") volMax[i] = 15;
    else {
      int top = 0;
      for (const auto& kv : mod.patterns) {
        int sub, ch, pat;
        if (sscanf(kv.first.c_str(), "%d,%d,%d", &sub, &ch, &pat) != 3 || ch != chans[i]) continue;
        for (const auto& rc : kv.second) if (rc.second.vol > top) top = rc.second.vol;
      }
      static const int cand[5] = {15, 31, 63, 127, 255};
      for (int c : cand) if (top <= c) { volMax[i] = c; break; }
    }
  }

  int patLen = song.patLen;
  int phrasesPerPattern = (patLen + 15) / 16;
  if (phrasesPerPattern > 16 || phrasesPerPattern < 1) {
    snprintf(projectFileError, 40, "Pattern length %d too long", patLen);
    return 1;
  }
  if (patLen % 16) warn("pattern length " + std::to_string(patLen) + " is not a multiple of 16: rows are padded");
  if (song.ordLen > PROJECT_MAX_LENGTH) {
    snprintf(projectFileError, 40, "More than 256 orders");
    return 1;
  }

  const int emptyPhrase = PROJECT_MAX_PHRASES - 1;
  std::map<std::string, int> phraseIds;
  std::map<std::string, int> chainIds;
  int nextPhrase = 0, nextChain = 0;
  std::map<int, int> grooves;       // speed -> groove id (0Fxx); groove 0 is the song groove
  int clamped = 0;
  std::map<std::string, int> dropped;
  std::vector<int> curVol(chans.size(), -1);
  // SID channels: instrument in use and the 1Exy parameters set since it was selected.
  std::vector<int> curIns(chans.size(), -1);
  std::vector<std::array<int, 6>> sidOv(chans.size(), std::array<int, 6>{-1, -1, -1, -1, -1, -1});

  for (int i = 0; i < PROJECT_MAX_LENGTH; i++)
    for (int t = 0; t < PROJECT_MAX_TRACKS; t++) p.song[i][t] = EMPTY_VALUE_16;

  for (int ord = 0; ord < song.ordLen; ord++) {
    for (size_t ci = 0; ci < chans.size(); ci++) {
      int c = chans[ci];
      const std::string& kindName = kinds[c].kind;
      int pat = c < (int)song.orders[ord].size() ? song.orders[ord][c] : 0;
      auto found = mod.patterns.find(patternKey(songIdx, c, pat));
      const FurRows* rows = found != mod.patterns.end() ? &found->second : NULL;
      std::vector<int> chain;
      for (int ph = 0; ph < phrasesPerPattern; ph++) {
        PhraseRow cells[16];
        bool blank = true;
        std::string text;
        for (int r = 0; r < 16; r++) {
          PhraseRow& dest = cells[r];
          initEmptyPhraseRow(&dest);
          int row = ph * 16 + r;
          if (row < patLen && rows) {
            auto rc = rows->find(row);
            if (rc != rows->end()) {
              const FurCell& cell = rc->second;
              bool realNote = false;
              if (cell.note >= 0) {
                if (cell.note >= 180 && cell.note <= 182) {
                  dest.note = NOTE_OFF;
                } else if (cell.note < 180) {
                  int ix = cell.note - 60;
                  if (ix < 0) { ix = 0; clamped++; }
                  if (ix >= p.pitchTable.length) { ix = p.pitchTable.length - 1; clamped++; }
                  dest.note = (uint8_t)ix;
                  realNote = true;
                }
              }
              bool isSid = kindName == "sid";
              if (isSid) {
                // Furnace keeps 1Exy values until another instrument is selected.
                if (cell.ins >= 0 && cell.ins != curIns[ci]) {
                  curIns[ci] = cell.ins;
                  sidOv[ci].fill(-1);
                }
                for (const auto& fx : cell.fx)
                  if (fx.cmd == 0x1E && fx.val >= 0 && (fx.val >> 4) <= 5)
                    sidOv[ci][fx.val >> 4] = (fx.val >> 4) < 4 ? (fx.val & 15) : (fx.val & 1);
              }
              bool hasOv = isSid && curIns[ci] >= 0 &&
                           std::any_of(sidOv[ci].begin(), sidOv[ci].end(), [](int x) { return x >= 0; });
              if (hasOv && (cell.ins >= 0 || realNote))
                dest.instrument = (uint8_t)conv.slotFor(curIns[ci], kindName, canSample, sidOv[ci].data());
              else if (cell.ins >= 0) dest.instrument = (uint8_t)conv.slotFor(cell.ins, kindName, canSample);
              bool hasVol = cell.vol >= 0;
              if (hasVol) {
                int v = (int)(cell.vol * 127.0 / volMax[ci] + 0.5);
                curVol[ci] = v < 127 ? v : 127;
              }
              if (realNote || hasVol) dest.volume = (uint16_t)(curVol[ci] >= 0 ? curVol[ci] : 127);
              int used = 0;
              for (const auto& fx : cell.fx) {
                if (fx.cmd < 0) continue;
                int val = fx.val < 0 ? 0 : fx.val;
                if (kindName == "sid" && fx.cmd == 0x1E && (val >> 4) <= 5) continue;
                FXCell m;
                if (!mapEffect(fx.cmd, val, grooves, &m)) {
                  char key[8];
                  snprintf(key, sizeof(key), "%02X", fx.cmd);
                  dropped[key]++;
                } else if (used < 3) {
                  dest.fx[used++] = m;
                } else {
                  dropped["extra"]++;
                }
              }
              blank = false;
            }
          }
          char buf[48];
          snprintf(buf, sizeof(buf), "%d,%d,%d", dest.note, dest.instrument, (int)dest.volume);
          text += buf;
          for (int e = 0; e < 3; e++) {
            snprintf(buf, sizeof(buf), ",%d:%d", (int)dest.fx[e].command, (int)dest.fx[e].value);
            text += buf;
          }
          text += ";";
        }
        int pid;
        if (blank) {
          pid = emptyPhrase;
        } else {
          auto it = phraseIds.find(text);
          if (it != phraseIds.end()) {
            pid = it->second;
          } else {
            if (nextPhrase >= emptyPhrase) {
              snprintf(projectFileError, 40, "Too many phrases");
              return 1;
            }
            pid = nextPhrase++;
            phraseIds[text] = pid;
            for (int r = 0; r < 16; r++) p.phrases[pid].rows[r] = cells[r];
          }
        }
        chain.push_back(pid);
      }
      std::string ckey;
      for (int pid : chain) ckey += std::to_string(pid) + ",";
      int cid;
      auto cit = chainIds.find(ckey);
      if (cit != chainIds.end()) {
        cid = cit->second;
      } else {
        if (nextChain > 254) {
          snprintf(projectFileError, 40, "Too many chains");
          return 1;
        }
        cid = nextChain++;
        chainIds[ckey] = cid;
        chainClear(&p.chains[cid]);
        for (size_t i = 0; i < chain.size(); i++) {
          p.chains[cid].rows[i].phrase = (uint16_t)chain[i];
          p.chains[cid].rows[i].transpose = 0;
        }
      }
      p.song[ord][ci] = (uint16_t)cid;
    }
  }
  if (clamped) warn(std::to_string(clamped) + " notes were outside the tracker's range and were clamped");
  if (!dropped.empty()) {
    std::string list;
    for (const auto& d : dropped) list += (list.empty() ? "" : ", ") + d.first + " (" + std::to_string(d.second) + ")";
    warn("effects not converted (Furnace command, count): " + list);
  }

  // Groove 0 from the speed pattern; 0Fxx effects use grooves 1-31.
  for (int g = 0; g < PROJECT_MAX_GROOVES; g++)
    for (int i = 0; i < 16; i++) p.grooves[g].speed[i] = EMPTY_VALUE_8;
  std::vector<int> speeds = song.hasSpeedPattern ? song.speedPattern : std::vector<int>{song.speed, song.speed2};
  for (int i = 0; i < 16 && i < (int)speeds.size(); i++) {
    int s = speeds[i];
    p.grooves[0].speed[i] = (uint8_t)(s < 1 ? 1 : (s > 255 ? 255 : s));
  }
  for (const auto& g : grooves) p.grooves[g.second].speed[0] = (uint8_t)g.first;

  // Instruments. Sample instruments come last because their WAV files are
  // numbered by rank among the sample slots.
  std::string wavDir;
  bool dirReady = false;
  int rank = 0;
  for (size_t s = 0; s < conv.slots.size(); s++) {
    Slot& slot = conv.slots[s];
    Instrument* inst = &p.instruments[s];
    const FurIns* fin = (slot.furIns >= 0 && slot.furIns < (int)furIns.size()) ? &furIns[slot.furIns] : NULL;
    bool done = false;
    if (slot.sample) {
      if (!dirReady) {
        wavDir = std::string(sampleDir);
        if (!wavDir.empty() && wavDir.back() != '/' && wavDir.back() != '\\') wavDir += PATH_SEPARATOR;
        wavDir += fileBase(path);
        dirReady = fileCreateDirectoryRecursive(wavDir.c_str()) == 0;
        if (!dirReady) warn("cannot create the sample folder, sample instruments use a default patch");
      }
      if (dirReady) {
        char name[32];
        snprintf(name, sizeof(name), "%c%03d.wav", PATH_SEPARATOR, rank);
        std::string wavPath = wavDir + name;
        FILE* f = fopen(wavPath.c_str(), "wb");
        bool wrote = f && fwrite(slot.wav.data(), 1, slot.wav.size(), f) == slot.wav.size();
        if (f) fclose(f);
        if (wrote && wavPath.size() <= (size_t)PROJECT_SAMPLE_PATH_LENGTH) {
          getInstrumentFunctions(InstrumentType::Sample).init(inst);
          InstrumentSample* sm = &inst->chip.sample;
          snprintf(sm->path, sizeof(sm->path), "%s", wavPath.c_str());
          char err[64];
          if (sampleLoadWav16(sm->path, sm, err, sizeof(err)) == 0) {
            sm->loopMode = slot.loop ? 1 : 0;
            sm->pitch = 0;
            sm->speedPercent = 100;
            sm->start = 0;
            sm->end = 255;
            setName(inst, slot.name);
            rank++;
            done = true;
          } else {
            getInstrumentFunctions(InstrumentType::Sample).free(inst);
          }
        }
        if (!done) warn("a sample could not be written, its instrument uses a default patch");
      }
    }
    if (!done) conv.buildKind(inst, slot.kind, fin, slot.furIns, slot.name);
    if (!done) {
      // Furnace macros advance once per song tick, the converter assumed 60 Hz.
      ChipProgram* prog = inst->type == InstrumentType::SID ? &inst->chip.sid.program
                          : inst->type == InstrumentType::SegaPSG ? &inst->chip.simpleChip.program : NULL;
      float hz = (song.tickRate > 0 && song.tickRate < 1000) ? song.tickRate : 50.0f;
      if (prog && prog->format == 1 && prog->size) prog->rate = (uint8_t)std::min(255, std::max(1, (int)(hz + 0.5f)));
    }
    if (!done && inst->type == InstrumentType::SID) {
      auto* v = inst->chip.sid.value;
      const int fields[6] = {sidAttack, sidDecay, sidSustain, sidRelease, sidRing, sidSync};
      for (int k = 0; k < 6; k++)
        if (slot.sidOv[k] >= 0) v[fields[k]] = (uint16_t)slot.sidOv[k];
    }
  }

  snprintf(p.title, sizeof(p.title), "%.24s", mod.name.c_str());
  snprintf(p.author, sizeof(p.author), "%.24s", mod.author.c_str());
  for (char* text : {p.title, p.author}) {
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) text[--len] = 0;
  }
  if (!song.name.empty() && p.title[0] == 0) snprintf(p.title, sizeof(p.title), "%.24s", song.name.c_str());
  p.tickRate = (song.tickRate > 0 && song.tickRate < 1000) ? song.tickRate : 50.0f;

  projectFree(project);
  *project = p;
  return 0;
}

int furListChannels(const char* path, int subsong, FurChannelInfo* channels, int maxChannels, int* count) {
  warningList().clear();
  *count = 0;
  std::vector<uint8_t> file;
  if (!path || !readWholeFile(path, file)) {
    snprintf(projectFileError, 40, "Cannot open Furnace file");
    return 1;
  }
  FurModule mod;
  const char* error = NULL;
  if (!furLoad(file, mod, &error)) {
    snprintf(projectFileError, 40, "%s", error ? error : "Not a Furnace module");
    return 1;
  }
  if (subsong < 0 || subsong >= (int)mod.songs.size()) {
    snprintf(projectFileError, 40, "No such Furnace subsong");
    return 1;
  }
  std::vector<ChannelKind> kinds = channelKinds(mod);
  std::vector<FurIns> furIns;
  for (const auto& blk : mod.insBlocks) furIns.push_back(parseInstrument(blk));
  std::vector<int> notes(kinds.size(), 0);
  for (const auto& kv : mod.patterns) {
    int sub, ch, pat;
    if (sscanf(kv.first.c_str(), "%d,%d,%d", &sub, &ch, &pat) != 3) continue;
    if (sub != subsong || ch < 0 || ch >= (int)kinds.size()) continue;
    for (const auto& rc : kv.second) if (rc.second.note >= 0) notes[ch]++;
  }
  std::vector<int> autoPick = autoChannels(mod, subsong, kinds, furIns);
  for (size_t c = 0; c < kinds.size() && *count < maxChannels; c++) {
    if (notes[c] == 0) continue;
    FurChannelInfo& info = channels[(*count)++];
    info.channel = (int)c;
    snprintf(info.name, sizeof(info.name), "%s %d", chipName(kinds[c].chipId).c_str(), kinds[c].local + 1);
    info.notes = notes[c];
    info.picked = 0;
    for (int a : autoPick) if (a == (int)c) info.picked = 1;
  }
  warningList().clear();
  return 0;
}

int projectLoadFur(Project* project, const char* path, const char* sampleDir) {
  return projectLoadFurChannels(project, path, sampleDir, NULL, 0, 0);
}
