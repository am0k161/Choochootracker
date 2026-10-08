#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "corelib_gfx.h"
#include "corelib_font.h"
#include "corelib_mainloop.h"
#include "app.h"
#include "common.h"

int main(int argv, char** args) {
  const char* captureScreen = NULL;
  const char* capturePath = NULL;
  const char* captureProject = NULL;
  const char* captureTheme = NULL;
  const char* captureFont = NULL;
  if (argv >= 4 && strcmp(args[1], "--screenshot") == 0) {
    captureScreen = args[2];
    capturePath = args[3];
    for (int i = 4; i < argv; i += 2) {
      if (i + 1 >= argv) { fprintf(stderr, "Missing value for %s\n", args[i]); return 2; }
      if (strcmp(args[i], "--project") == 0) captureProject = args[i + 1];
      else if (strcmp(args[i], "--theme") == 0) captureTheme = args[i + 1];
      else if (strcmp(args[i], "--font") == 0) captureFont = args[i + 1];
      else { fprintf(stderr, "Unknown screenshot option: %s\n", args[i]); return 2; }
    }
  } else if (argv != 1) {
    fprintf(stderr, "Usage: %s [--screenshot SCREEN OUTPUT.png [--project FILE.cct] [--theme FILE.cth] [--font FILE.cnfont]]\n", args[0]);
    return 2;
  }
  settingsLoad();
  if (captureTheme && loadTheme(captureTheme) != 0) { fprintf(stderr, "Could not load theme: %s\n", captureTheme); return 1; }
  if (captureFont) { strncpy(appSettings.fontPath, captureFont, PATH_LENGTH); appSettings.fontPath[PATH_LENGTH] = 0; }

  // Load custom font before gfxSetup so it uses the correct font
  if (appSettings.fontPath[0] != '\0') {
    Font* font = fontLoad(appSettings.fontPath);
    if (font) {
      fontSetCurrent(font);
    } else {
      appSettings.fontPath[0] = '\0';
      fontSetCurrent(NULL);
    }
  }

  if (captureScreen) gfxSetCaptureSize(1920, 1080);
  if (gfxSetup(&appSettings.screenWidth, &appSettings.screenHeight) != 0) return 1;

  appSetup();
  if (captureScreen) {
    int result = appCaptureScreen(captureScreen, captureProject) || gfxCapturePNG(capturePath);
    appCleanup();
    gfxCleanup();
    mainLoopQuit();
    return result;
  }
  mainLoopRun(appDraw, appOnEvent);
#ifndef WEB_BUILD
  appCleanup();
  gfxCleanup();
  mainLoopQuit();
#endif

  return 0;
}

extern "C" int SDL_main(int argv, char** args) {
  return main(argv, args);
}
