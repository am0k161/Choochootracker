#include "screen_project.h"
#include "common.h"
#include "corelib/corelib_file.h"
#include "corelib_gfx.h"
#include "utils.h"
#include "chipnomad_lib.h"
#include "project_utils.h"
#include "pitch_table_utils.h"
#include "version.h"
#include "audio_manager.h"
#include "file_browser.h"
#include "export_path.h"
#include "import/import_vt2.h"
#include "import/import_midi.h"
#include "import/import_m8s.h"
#include "string_utils.h"
#include "midi/midi_router.h"
#include <string.h>
#include <strings.h>

#ifdef WEB_BUILD
#include <emscripten/emscripten.h>
#endif

static int isCharEdit = 0;
static const AppScreen* projectReturnScreen = &screenProject;
static char* editingString = NULL;
static int editingStringLength = 0;
// Tempo is edited/displayed in tenths of BPM (125.0 BPM = 1250). The
// project's canonical tickRate stays in Hz (see project_io.cpp's
// "- Frame rate" format): BPM = tickRate * 60/24 = tickRate * 2.5, so
// tenths = tickRate * 25 - exact for every Hz value the old UI could
// produce, and old projects load/save byte-identically.
static uint16_t bpmTenths = 1250;
// Mirror of appSettings.midiClockMode for the Clock source row.
static uint8_t clockSourceMirror = 0;

// BPM display range: tickRate 1-200 Hz maps to 2.5-500 BPM.
#define BPM_TENTHS_MIN (25)
#define BPM_TENTHS_MAX (5000)
#define BPM_TENTHS_DEFAULT (1250) // 125.0 BPM = the old 50 Hz default

int projectLoadFromPath(const char* path) {
  if (!path) {
    return 1;
  }

  // Store the directory path from the selected file
  const char* lastSeparator = strrchr(path, PATH_SEPARATOR);
  if (lastSeparator) {
    int pathLen = lastSeparator - path;
    if (pathLen > 0 && pathLen < PATH_LENGTH) {
      strncpy(appSettings.projectPath, path, pathLen);
      appSettings.projectPath[pathLen] = '\0';
    }
  } else {
    appSettings.projectPath[0] = '\0';
  }

  // Check file extension to determine loader
  const char* ext = strrchr(path, '.');
  int loadResult = -1;
  Project replacement;
  projectInitAY(&replacement);

  if (ext != NULL) {
    if (strcasecmp(ext, ".vt2") == 0) {
      // Load VT2 file
      loadResult = projectLoadVT2(&replacement, path);
    } else if (strcasecmp(ext, ".mid") == 0 || strcasecmp(ext, ".midi") == 0) {
      // Import a Standard MIDI File as a new project
      loadResult = projectLoadMidi(&replacement, path);
    } else if (strcasecmp(ext, ".m8s") == 0) {
      // Import a Dirtywave M8 song (structure and notes only)
      loadResult = projectLoadM8S(&replacement, path);
    } else if (strcasecmp(ext, ".cct") == 0) {
      // Load ChooChooTracker native format
      loadResult = projectLoad(&replacement, path);
    } else {
      // Try native format by default
      loadResult = projectLoad(&replacement, path);
    }
  } else {
    // No extension, try native format
    loadResult = projectLoad(&replacement, path);
  }

  if (loadResult == 0) {
    audioManager.replaceProject(&replacement);
    autosaveLoadFailed = 0;
    projectModified = 0; // Clear modified flag after loading

    // Store filename without extension
    extractFilenameWithoutExtension(path, appSettings.projectFilename, FILENAME_LENGTH + 1);
    // The loaded project's export folder (if any) is adopted on next export
    exportResetFolderTracking();
    settingsSave();

    // Reset all screen states (including song position)
    screensInitAll();
    // Clear FX states for all tracks
    // TODO: Make it a cleaner solution and have it somewhere in chipnomad_lib
    for (int i = 0; i < PROJECT_MAX_TRACKS; i++) {
      chipnomadQueuePlaybackClearTrackFX(chipnomadState, i);
    }
  } else {
    projectFree(&replacement);
    screenMessage(MESSAGE_TIME_ERROR, "%s", projectFileError);
  }

  return loadResult;
}

static void onProjectLoaded(const char* path) {
  if (projectLoadFromPath(path) == 0 && projectReturnScreen == &screenTitle)
    screenSetup(&screenSong, 0);
  else
    screenSetup(projectReturnScreen, 0);
}

#ifdef WEB_BUILD
extern "C" EMSCRIPTEN_KEEPALIVE int webLoadProject(const char* path) {
  return projectLoadFromPath(path);
}
#endif

static void onProjectSaved(const char* folderPath) {
  char fullPath[2048];
  snprintf(fullPath, sizeof(fullPath), "%s%s%s.cct", folderPath, PATH_SEPARATOR_STR, appSettings.projectFilename);

  if (projectSave(&chipnomadState->project, fullPath) == 0) {
    // Save the directory path
    projectModified = 0; // Clear modified flag after saving
    strncpy(appSettings.projectPath, folderPath, PATH_LENGTH);
    appSettings.projectPath[PATH_LENGTH] = 0;
    // If exports were made under a previous project name, move that folder
    // to match the name the project was just saved with
    exportSyncFolderWithProjectName();
    settingsSave();
  }
  screenSetup(&screenProject, 0);
}

static void onProjectCancelled(void) {
  screenSetup(projectReturnScreen, 0);
}

static void doLoadProject(void) {
  fileBrowserSetup("LOAD PROJECT", ".cct,.vt2,.mid,.midi,.m8s", appSettings.projectPath,
    onProjectLoaded, onProjectCancelled);
  screenSetup(&screenFileBrowser, 0);
}

static void doNewProject(void) {
  Project replacement;
  projectInitAY(&replacement);
  audioManager.replaceProject(&replacement);
  projectModified = 0;
  appSettings.projectFilename[0] = 0;
  exportResetFolderTracking();
  settingsSave();
  screensInitAll();
  screenSetup(projectReturnScreen, 0);
}

void projectOpenFromScreen(const AppScreen* returnScreen) {
  projectReturnScreen = returnScreen ? returnScreen : &screenProject;
  doLoadProject();
}

void projectOpenFromScreenAtPath(const AppScreen* returnScreen, const char* path) {
  projectReturnScreen = returnScreen ? returnScreen : &screenProject;
  fileBrowserSetup("LOAD PROJECT", ".cct,.vt2,.mid,.midi,.m8s", path,
    onProjectLoaded, onProjectCancelled);
  screenSetup(&screenFileBrowser, 0);
}

void projectCreateNewFromScreen(const AppScreen* returnScreen) {
  projectReturnScreen = returnScreen ? returnScreen : &screenProject;
  doNewProject();
}

static void cancelConfirm(void) {
  screenSetup(&screenProject, 0);
}

static void drawRowHeader(int row, CellState state);
static void drawColHeader(int col, CellState state);

static ScreenData screenProjectCommon = {
  .rows = SCR_PROJECT_ROWS,
  .cursorRow = 0,
  .cursorCol = 0,
  .topRow = 0,
  .selectMode = -1,
  .selectStartRow = 0,
  .selectStartCol = 0,
  .selectAnchorRow = 0,
  .selectAnchorCol = 0,
  .playbackLevel = ScreenPlaybackLevel::none,
  .getColumnCount = projectCommonColumnCount,
  .drawStatic = projectCommonDrawStatic,
  .drawCursor = projectCommonDrawCursor,
  .drawSelection = NULL,
  .drawRowHeader = drawRowHeader,
  .drawColHeader = drawColHeader,
  .drawField = projectCommonDrawField,
  .onEdit = projectCommonOnEdit,
  .onInput = NULL,
  .onRawInput = NULL,
  .isCellValid = NULL,
  .getLoopRange = NULL,
};

static ScreenData* projectScreen(void) {
  ScreenData* data = &screenProjectCommon;
  if (chipnomadState->project.chipType == ChipType::AY) {
    data = &screenProjectAY;
  }
  data->drawRowHeader = drawRowHeader;
  data->drawColHeader = drawColHeader;

  return data;
}

static void setup(int input) {
  isCharEdit = 0;
  editingString = NULL;
  editingStringLength = 0;
  // Round to tenths: 50 Hz -> 1250, 59 Hz -> 1475 ("147.5 BPM").
  int tenths = (int)(chipnomadState->project.tickRate * 25.0f + 0.5f);
  if (tenths < BPM_TENTHS_MIN) tenths = BPM_TENTHS_MIN;
  if (tenths > BPM_TENTHS_MAX) tenths = BPM_TENTHS_MAX;
  bpmTenths = (uint16_t)tenths;
  clockSourceMirror = appSettings.midiClockMode ? 1 : 0;
}

static void fullRedraw(void) {
  ScreenData* screen = projectScreen();
  screenFullRedraw(screen);
}

static void draw(void) {
  // While slaved to MIDI clock the tempo field shows the live followed
  // tempo (app.cpp keeps project.tickRate updated); fullRedraw only runs on
  // navigation/edit, so refresh the field here every frame. The cursor
  // overlay is redrawn by the framework after draw() (see screenDraw), so
  // clearing the field area here is safe.
  if (clockSourceMirror) {
    ScreenOverlayCoordinates overlay;
    const ColorScheme cs = appSettings.colorScheme;
    gfxSetFgColor(cs.textInfo);
    gfxClearRect(13, 11, 27, 1);
    gfxPrintf(13, 11, "%u.%u BPM", bpmTenths / 10, bpmTenths % 10);
  }
}

///////////////////////////////////////////////////////////////////////////////
//
// Common part of the form
//

static void drawRowHeader(int row, CellState state) {}
static void drawColHeader(int col, CellState state) {}

int projectCommonColumnCount(int row) {
  if (row == 0) {
    return 5; // Load, save, new, export, manage
  } else if (row >= 1 && row <= 3) {
    return 24; // File, title, author
  } else if (row == 4) {
    return 1; // Linear pitch
  } else if (row == 5) {
    return 1; // Clock source
  } else if (row == 6) {
    return 1; // Tempo (BPM)
  }
  return 1; // Default value
}

void projectCommonDrawStatic(void) {
  const ColorScheme cs = appSettings.colorScheme;

  gfxSetFgColor(cs.textTitles);
  gfxPrint(0, 0, "PROJECT");

  gfxSetFgColor(cs.textDefault);
  gfxPrint(8, 0, appTitle);
  gfxPrintf(0, 1, "v%s (%s)", appVersion, appBuild);

  gfxPrint(0, 5, "File");
  gfxPrint(0, 6, "Title");
  gfxPrint(0, 7, "Author");

  gfxPrint(0, 9, "Linear pitch");
  gfxPrint(0, 10, "Clock source");
  gfxPrint(0, 11, "Tempo");
}

void projectCommonDrawCursor(int col, int row) {
  if (row == 0) {
    if (col == 0) {
      gfxCursor(7, 3, 4); // Load
    } else if (col == 1) {
      gfxCursor(12, 3, 4); // Save
    } else if (col == 2) {
      gfxCursor(17, 3, 3); // New
    } else if (col == 3) {
      gfxCursor(21, 3, 6); // Export
    } else if (col == 4) {
      gfxCursor(28, 3, 6); // Manage
    }
  } else if (row >= 1 && row <= 3) {
    // Text fields: file name, title, author
    gfxCursor(7 + col, 4 + row, 1);
  } else if (row == 4) {
    // Linear pitch
    gfxCursor(13, 9, 3);
  } else if (row == 5) {
    // Clock source ("Internal" / "Midi Clock")
    gfxCursor(13, 10, 10);
  } else if (row == 6) {
    // Tempo ("500.0 BPM")
    gfxCursor(13, 11, 8);
  }
}

void projectCommonDrawField(int col, int row, CellState state) {
  gfxSetFgColor(state == CellState::focus ? appSettings.colorScheme.textValue : appSettings.colorScheme.textDefault);

  if (row == 0) {
    if (col == 0) {
      gfxPrint(7, 3, "Load");
    } else if (col == 1) {
      gfxPrint(12, 3, "Save");
    } else if (col == 2) {
      gfxPrint(17, 3, "New");
    } else if (col == 3) {
      gfxPrint(21, 3, "Export");
    } else if (col == 4) {
      gfxPrint(28, 3, "Manage");
    }
  } else if (row == 1) {
    // File name
    gfxClearRect(7, 5, FILENAME_LENGTH, 1);
    gfxPrintf(7, 5, "%s", appSettings.projectFilename);
  } else if (row == 2) {
    // Title
    gfxClearRect(7, 6, PROJECT_TITLE_LENGTH, 1);
    gfxPrintf(7, 6, "%s", chipnomadState->project.title);
  } else if (row == 3) {
    // Author
    gfxClearRect(7, 7, PROJECT_TITLE_LENGTH, 1);
    gfxPrintf(7, 7, "%s", chipnomadState->project.author);
  } else if (row == 4) {
    // Linear pitch
    gfxPrint(13, 9, chipnomadState->project.linearPitch ? "ON " : "OFF");
  } else if (row == 5) {
    // Clock source
    gfxClearRect(13, 10, 10, 1);
    gfxPrint(13, 10, clockSourceMirror ? "Midi Clock" : "Internal");
  } else if (row == 6) {
    // Tempo in BPM (greyed while slaved to MIDI clock - it's read-only
    // there, driven by the incoming clock).
    gfxClearRect(13, 11, 27, 1);
    if (clockSourceMirror) {
      gfxSetFgColor(state == CellState::focus ? appSettings.colorScheme.textInfo : appSettings.colorScheme.textEmpty);
      gfxPrintf(13, 11, "%u.%u BPM", bpmTenths / 10, bpmTenths % 10);
    } else {
      gfxPrintf(13, 11, "%u.%u BPM", bpmTenths / 10, bpmTenths % 10);
    }
  }
}

int projectCommonOnEdit(int col, int row, enum CellEditAction action) {
  int handled = 0;

  if (row == 0) {
    // Load/Save/New/Export
    if (col == 0) {
      // Load project
      projectReturnScreen = &screenProject;
      if (projectModified) {
        confirmSetup("Lose unsaved changes?", doLoadProject, cancelConfirm);
        screenSetup(&screenConfirm, 0);
      } else {
        doLoadProject();
      }
    } else if (col == 1) {
      // Save project - check filename first
      if (strlen(appSettings.projectFilename) == 0) {
        screenMessage(MESSAGE_TIME, "Enter filename");
        handled = 1;
      } else {
        fileBrowserSetupFolderMode("SAVE PROJECT", appSettings.projectPath, appSettings.projectFilename, ".cct", onProjectSaved, onProjectCancelled);
        screenSetup(&screenFileBrowser, 0);
      }
    } else if (col == 2) {
      // New project
      projectReturnScreen = &screenProject;
      if (projectModified) {
        confirmSetup("Lose unsaved changes?", doNewProject, cancelConfirm);
        screenSetup(&screenConfirm, 0);
      } else {
        doNewProject();
      }
      handled = 1;
    } else if (col == 3) {
      // Export - go to export screen
      screenSetup(&screenExport, 0);
      handled = 0;
    } else if (col == 4) {
      // Manage - go to manage screen
      screenSetup(&screenManage, 0);
      handled = 0;
    }
  } else if (row == 1) {
    // File name
    int res = editCharacter(action, appSettings.projectFilename, col, FILENAME_LENGTH);
    if (res == 1) {
      isCharEdit = 1;
      editingString = appSettings.projectFilename;
      editingStringLength = FILENAME_LENGTH;
    } else if (res > 1) {
      handled = 1;
    }
  } else if (row == 2) {
    // Title
    int res = editCharacter(action, chipnomadState->project.title, col, PROJECT_TITLE_LENGTH);
    if (res == 1) {
      isCharEdit = 1;
      editingString = chipnomadState->project.title;
      editingStringLength = PROJECT_TITLE_LENGTH;
    } else if (res > 1) {
      handled = 1;
    }
  } else if (row == 3) {
    // Author
    int res = editCharacter(action, chipnomadState->project.author, col, PROJECT_TITLE_LENGTH);
    if (res == 1) {
      isCharEdit = 1;
      editingString = chipnomadState->project.author;
      editingStringLength = PROJECT_TITLE_LENGTH;
    } else if (res > 1) {
      handled = 1;
    }
  } else if (row == 4) {
    // Linear pitch (ON/OFF)
    handled = edit8noLast(action, &chipnomadState->project.linearPitch, 1, 0, 1);
    if (handled) {
      projectModified = 1;
      chipnomadQueuePlaybackStop(chipnomadState);
      reinitializePitchTable(&chipnomadState->project);
    }
  } else if (row == 5) {
    // Clock source: Internal / Midi Clock. Persisted in settings.txt
    // (appSettings.midiClockMode) and applied to the router live; the
    // audio thread picks it up through the regular project refresh.
    action = convertMultiAction(action);
    switch (action) {
      case CellEditAction::tap:
      case CellEditAction::doubleTap:
        clockSourceMirror = clockSourceMirror ? 0 : 1;
        handled = 1;
        break;
      case CellEditAction::clear:
        clockSourceMirror = 0;
        handled = 1;
        break;
      case CellEditAction::increase:
      case CellEditAction::increaseBig:
        clockSourceMirror = 1;
        handled = 1;
        break;
      case CellEditAction::decrease:
      case CellEditAction::decreaseBig:
        clockSourceMirror = 0;
        handled = 1;
        break;
      default:
        break;
    }
    if (handled) {
      appSettings.midiClockMode = clockSourceMirror;
      midiRouterSetClockMode(chipnomadState->midiRouter,
        clockSourceMirror ? MidiClockMode::slave : MidiClockMode::off);
    }
  } else if (row == 6) {
    // Tempo in BPM. Fine steps are 1 BPM (10 tenths), coarse 5 BPM (50
    // tenths); clear restores the old default 50 Hz = 125.0 BPM. While
    // slaved to MIDI clock the tempo is driven by the incoming clock, so
    // edits are swallowed (the field shows the live followed tempo).
    if (clockSourceMirror) {
      handled = 1;
    } else {
      action = convertMultiAction(action);
      switch (action) {
        case CellEditAction::tap:
        case CellEditAction::doubleTap:
          bpmTenths += 1;
          if (bpmTenths > BPM_TENTHS_MAX) bpmTenths = BPM_TENTHS_MIN;
          handled = 1;
          break;
        case CellEditAction::clear:
          bpmTenths = BPM_TENTHS_DEFAULT;
          handled = 1;
          break;
        case CellEditAction::increase:
          if (bpmTenths < BPM_TENTHS_MAX) bpmTenths += 10;
          handled = 1;
          break;
        case CellEditAction::decrease:
          if (bpmTenths > BPM_TENTHS_MIN) bpmTenths -= 10;
          handled = 1;
          break;
        case CellEditAction::increaseBig:
          bpmTenths = bpmTenths > BPM_TENTHS_MAX - 50 ? BPM_TENTHS_MAX : bpmTenths + 50;
          handled = 1;
          break;
        case CellEditAction::decreaseBig:
          bpmTenths = bpmTenths < BPM_TENTHS_MIN + 50 ? BPM_TENTHS_MIN : bpmTenths - 50;
          handled = 1;
          break;
        default:
          break;
      }
      if (handled) {
        // Backwards compatible writeback: the project file keeps storing
        // Hz ("- Frame rate"), tenths * 0.04 = tenths / 25 in Hz.
        chipnomadState->project.tickRate = (float)bpmTenths * 0.04f;
        projectModified = 1;
      }
    }
  }

  return handled;
}


///////////////////////////////////////////////////////////////////////////////
//
// Input handling
//

static int inputScreenNavigation(int keys, int tapCount) {
  if (keys == keyOpt || keys == (keyDown | keyShift)) {
    screenSetup(&screenSong, 0);
    return 1;
  }
  return 0;
}

static int onInput(int isKeyDown, int keys, int tapCount) {
  if (isCharEdit) {
    ScreenData* screen = projectScreen();
    char result = charEditInput(keys, tapCount, editingString, screen->cursorCol, editingStringLength);

    if (result) {
      projectModified = 1;
      isCharEdit = 0;
      if (screen->cursorCol < editingStringLength - 1) screen->cursorCol++;
      editingString = NULL;
      editingStringLength = 0;
      fullRedraw();
    }
  } else {
    if (inputScreenNavigation(keys, tapCount)) return 1;

    ScreenData* screen = projectScreen();
    if (screenInput(screen, isKeyDown, keys, tapCount)) return 1;
  }
  return 0;
}

#ifdef DESKTOP_BUILD

// Key jazz text entry (see keyJazzTextHandleRawKey): filename, title, author.
int projectKeyJazzTextField(KeyJazzTextField* field) {
  ScreenData* screen = projectScreen();
  field->screen = screen;
  field->row = screen->cursorRow;
  field->popupOpen = isCharEdit;
  field->marksProjectModified = 1;
  field->str = NULL;
  if (screen->cursorRow == 1) { field->str = appSettings.projectFilename; field->maxLen = FILENAME_LENGTH; }
  else if (screen->cursorRow == 2) { field->str = chipnomadState->project.title; field->maxLen = PROJECT_TITLE_LENGTH; }
  else if (screen->cursorRow == 3) { field->str = chipnomadState->project.author; field->maxLen = PROJECT_TITLE_LENGTH; }
  return 1;
}

#endif // DESKTOP_BUILD

static ScreenPlaybackLevel getPlaybackLevel(void) {
  return ScreenPlaybackLevel::song;
}

const AppScreen screenProject = {
  .init = NULL,
  .setup = setup,
  .fullRedraw = fullRedraw,
  .draw = draw,
  .onInput = onInput,
  .getPlaybackLevel = getPlaybackLevel,
};
