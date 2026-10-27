#ifndef __CHIPNOMAD_LIB__EXPORT_EXPORT_FUR_H__
#define __CHIPNOMAD_LIB__EXPORT_EXPORT_FUR_H__

#include "../project.h"

// Exports a project as a Furnace module (.fur, format 157, readable by
// Furnace 0.6pre5 and 0.6.8.3; written uncompressed).
//
// The export is lossy: the tracker picks the sound per instrument, Furnace
// per channel. Each used track is given to the chip that matches the
// instruments it plays most (YM2612, YM2151, OPL2, OPL3, SID, else AY-3-8910)
// and gets a default patch where an instrument belongs to another engine.
// Notes, volumes, chain transposition, the groove 0 speeds and the effects
// ARP, PVB, PAN, OFF and DEL are kept. Samples, the other engines (Braids,
// Plaits...), the other effects and tables are not exported.
//
// The tracker's chain lengths can differ per track, Furnace patterns cannot:
// shorter tracks are padded with empty rows. OPL3 four-operator patches only
// sound on the first channel of a Furnace channel pair, so tracks that play
// one are placed there.
//
// Returns 0 on success. On failure returns non-zero and sets
// projectExportFurError.
extern char projectExportFurError[48];
int projectExportFur(Project* project, const char* outputPath);

// What was lost or approximated by the last export, one line per item
// separated by newlines ("" when nothing). Valid until the next export.
const char* projectExportFurWarnings(void);

#endif // __CHIPNOMAD_LIB__EXPORT_EXPORT_FUR_H__
