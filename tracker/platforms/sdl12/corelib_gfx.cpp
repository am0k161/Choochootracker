#include <SDL/SDL.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include "version.h"
#include "corelib_gfx.h"
#include "corelib_font.h"
#include "../../src/background.h"

#define WINDOW_WIDTH (640)
#define WINDOW_HEIGHT (480)
#define PRINT_BUFFER_SIZE (256)

#define CHAR_X(x) ((x) * fontW * 8)
#define CHAR_Y(y) (((y) + gfxGetContentRowOffset()) * fontH)

extern uint8_t font16x24[];

SDL_Surface *sdlScreen;
static uint32_t fgColor = 0;
static uint32_t bgColor = 0;
static uint32_t cursorColor = 0;
static int fgR = 255, fgG = 255, fgB = 255;
static int bgR = 0, bgG = 0, bgB = 0;
static int cursorR = 255, cursorG = 255, cursorB = 255;
static uint8_t* font = NULL;
static char printBuffer[PRINT_BUFFER_SIZE];
static int fontH;
static int fontW;
static int isDirty;
static const FontResolution* currentResolution = NULL;

// Font surface optimization
static SDL_Surface* charSurfaces[95]; // ASCII 32-126

static char charBuffer[80];

static void createCharSurfaces(void) {
  if (!currentResolution || !currentResolution->data) return;

  font = (uint8_t*)currentResolution->data;
  fontW = (currentResolution->charWidth + 7) / 8;
  fontH = currentResolution->charHeight;

  // Create 8-bit character surfaces
  for (int ch = 0; ch < 95; ch++) {
    charSurfaces[ch] = SDL_CreateRGBSurface(SDL_SWSURFACE, fontW * 8, fontH, 8, 0, 0, 0, 0);
    SDL_SetColorKey(charSurfaces[ch], SDL_SRCCOLORKEY, 0);

    for (int l = 0; l < fontH; l++) {
      for (int c = 0; c < fontW; c++) {
        uint8_t fontByte = font[ch * fontW * fontH + l * fontW + c];
        uint8_t mask = 0x80;

        for (int b = 0; b < 8; b++) {
          ((Uint8 *)charSurfaces[ch]->pixels)[l * charSurfaces[ch]->w + (c * 8 + b)] = (fontByte & mask) ? 1 : 0;
          mask >>= 1;
        }
      }
    }
  }
}

#ifdef MIYOOPORTS_BUILD
static SDL_Surface* offscreenSurface = NULL;
#endif
struct GfxImage {
  SDL_Surface* surface;
  int width;
  int height;
};

static SDL_Surface* titleSurface = NULL;
static SDL_Surface* backgroundCanvas = NULL;
static int backgroundBaseColor = 0;

static SDL_Surface* trackerSurface(void) {
#ifdef MIYOOPORTS_BUILD
  return offscreenSurface;
#else
  return sdlScreen;
#endif
}

static void freeBackground(void) {
  if (backgroundCanvas) SDL_FreeSurface(backgroundCanvas);
  backgroundCanvas = NULL;
}

static int ensureBackground(void) {
  SDL_Surface* target = trackerSurface();
  if (!target) return 0;
  if (backgroundCanvas && backgroundCanvas->w == target->w && backgroundCanvas->h == target->h) return 1;
  freeBackground();
  backgroundCanvas = SDL_CreateRGBSurface(SDL_SWSURFACE, target->w, target->h, 32,
    target->format->Rmask, target->format->Gmask, target->format->Bmask, target->format->Amask);
  return backgroundCanvas != NULL;
}

static void drawBackgroundRect(SDL_Rect rect) {
  SDL_Surface* target = trackerSurface();
  if (backgroundCanvas) SDL_BlitSurface(backgroundCanvas, &rect, target, &rect);
  else SDL_FillRect(target, &rect, SDL_MapRGB(target->format,
    backgroundBaseColor >> 16 & 255, backgroundBaseColor >> 8 & 255, backgroundBaseColor & 255));
}

int gfxSetup(int *screenWidth, int *screenHeight) {
  if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
    printf("SDL2 Initialization Error: %s\n", SDL_GetError());
    return 1;
  }

#ifdef MIYOOPORTS_BUILD
  sdlScreen = SDL_SetVideoMode(WINDOW_WIDTH, WINDOW_HEIGHT, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
  if (!sdlScreen) {
    printf("SDL1.2 Set Video Mode Error: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  offscreenSurface = SDL_CreateRGBSurface(SDL_SWSURFACE, WINDOW_WIDTH, WINDOW_HEIGHT, 32,
    sdlScreen->format->Rmask, sdlScreen->format->Gmask, sdlScreen->format->Bmask, sdlScreen->format->Amask);
  if (!offscreenSurface) {
    printf("Failed to create offscreen surface: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
#else
  sdlScreen = SDL_SetVideoMode(WINDOW_WIDTH, WINDOW_HEIGHT, 32, SDL_HWSURFACE);
  if (!sdlScreen) {
    printf("SDL1.2 Set Video Mode Error: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
#endif

  sprintf(charBuffer, "%s v%s (%s)", appTitle, appVersion, appBuild);
  SDL_WM_SetCaption(charBuffer, NULL);

  currentResolution = fontSelectResolution(fontGetCurrent(), WINDOW_WIDTH, WINDOW_HEIGHT);
  if (!currentResolution) {
    currentResolution = &fontGetDefault()->resolutions[1]; // Use 16x24
  }

  createCharSurfaces();
  isDirty = 1;

  return 0;
}

void gfxCleanup(void) {
  freeBackground();
  for (int i = 0; i < 95; i++) {
    if (charSurfaces[i]) SDL_FreeSurface(charSurfaces[i]);
  }
  SDL_FreeSurface(sdlScreen);
#ifdef MIYOOPORTS_BUILD
  if (offscreenSurface) {
    SDL_FreeSurface(offscreenSurface);
    offscreenSurface = NULL;
  }
#endif
  if (titleSurface) {
    SDL_FreeSurface(titleSurface);
    titleSurface = NULL;
  }
}

void gfxSetFgColor(int rgb) {
  fgR = (rgb & 0xff0000) >> 16;
  fgG = (rgb & 0xff00) >> 8;
  fgB = rgb & 0xff;
  fgColor = SDL_MapRGB(sdlScreen->format, fgR, fgG, fgB);
}

void gfxSetBgColor(int rgb) {
  bgR = (rgb & 0xff0000) >> 16;
  bgG = (rgb & 0xff00) >> 8;
  bgB = rgb & 0xff;
  bgColor = SDL_MapRGB(sdlScreen->format, bgR, bgG, bgB);
}

void gfxBackgroundSetBaseColor(int rgb) { backgroundBaseColor = rgb; }

void gfxBackgroundSetSolid(void) {
  freeBackground();
  isDirty = 1;
}

static uint32_t blendBackgroundPixel(uint32_t pixel, int base, int intensity) {
  int r = pixel >> 16 & 255, g = pixel >> 8 & 255, b = pixel & 255;
  intensity = intensity < 0 ? 0 : intensity > 100 ? 100 : intensity;
  r = ((base >> 16 & 255) * (100 - intensity) + r * intensity) / 100;
  g = ((base >> 8 & 255) * (100 - intensity) + g * intensity) / 100;
  b = ((base & 255) * (100 - intensity) + b * intensity) / 100;
  return 0xff000000u | (uint32_t)(r << 16 | g << 8 | b);
}

int gfxBackgroundLoadBMP(const char* path, int layout, int tint, int intensity) {
  SDL_Surface* loaded = path && path[0] ? SDL_LoadBMP(path) : NULL;
  if (!loaded || !ensureBackground()) { if (loaded) SDL_FreeSurface(loaded); return 0; }
  const int count = loaded->w * loaded->h;
  uint32_t* source = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
  if (!source) { SDL_FreeSurface(loaded); return 0; }
  if (SDL_LockSurface(loaded) != 0) { free(source); SDL_FreeSurface(loaded); return 0; }
  for (int y = 0; y < loaded->h; ++y) for (int x = 0; x < loaded->w; ++x) {
    Uint8 r, g, b;
    Uint8* address = (Uint8*)loaded->pixels + y * loaded->pitch + x * loaded->format->BytesPerPixel;
    Uint32 raw;
    if (loaded->format->BytesPerPixel == 4) raw = *(Uint32*)address;
    else if (loaded->format->BytesPerPixel == 3) {
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
      raw = address[0] << 16 | address[1] << 8 | address[2];
#else
      raw = address[0] | address[1] << 8 | address[2] << 16;
#endif
    } else if (loaded->format->BytesPerPixel == 2) raw = *(Uint16*)address;
    else raw = *address;
    SDL_GetRGB(raw, loaded->format, &r, &g, &b);
    source[y * loaded->w + x] = 0xff000000u | r << 16 | g << 8 | b;
  }
  SDL_UnlockSurface(loaded);
  const bool colorize = tint == (int)BackgroundTint::tinted ||
    (tint == (int)BackgroundTint::automatic && backgroundPixelsAreMonochrome(source, count));
  for (int i = 0; i < count; ++i)
    source[i] = colorize ? backgroundTintPixel(source[i], backgroundBaseColor, intensity)
                         : blendBackgroundPixel(source[i], backgroundBaseColor, intensity);
  if (SDL_LockSurface(backgroundCanvas) == 0) {
    for (int y = 0; y < backgroundCanvas->h; ++y) for (int x = 0; x < backgroundCanvas->w; ++x) {
      int sx = layout == (int)BackgroundLayout::tile ? x % loaded->w : x * loaded->w / backgroundCanvas->w;
      int sy = layout == (int)BackgroundLayout::tile ? y % loaded->h : y * loaded->h / backgroundCanvas->h;
      uint32_t pixel = source[sy * loaded->w + sx];
      Uint32 mapped = SDL_MapRGB(backgroundCanvas->format, pixel >> 16 & 255, pixel >> 8 & 255, pixel & 255);
      *(Uint32*)((Uint8*)backgroundCanvas->pixels + y * backgroundCanvas->pitch + x * 4) = mapped;
    }
    SDL_UnlockSurface(backgroundCanvas);
  }
  free(source);
  SDL_FreeSurface(loaded);
  isDirty = 1;
  return 1;
}

void gfxBackgroundSetPixels(const uint32_t* source, int width, int height) {
  if (!source || width <= 0 || height <= 0 || !ensureBackground()) return;
  if (SDL_LockSurface(backgroundCanvas) != 0) return;
  for (int y = 0; y < backgroundCanvas->h; ++y) for (int x = 0; x < backgroundCanvas->w; ++x) {
    uint32_t pixel = source[(y * height / backgroundCanvas->h) * width + x * width / backgroundCanvas->w];
    *(Uint32*)((Uint8*)backgroundCanvas->pixels + y * backgroundCanvas->pitch + x * 4) =
      SDL_MapRGB(backgroundCanvas->format, pixel >> 16 & 255, pixel >> 8 & 255, pixel & 255);
  }
  SDL_UnlockSurface(backgroundCanvas);
  isDirty = 1;
}

void gfxSetCursorColor(int rgb) {
  cursorR = (rgb & 0xff0000) >> 16;
  cursorG = (rgb & 0xff00) >> 8;
  cursorB = rgb & 0xff;
  cursorColor = SDL_MapRGB(sdlScreen->format, cursorR, cursorG, cursorB);
}

// This renderer always draws the cursor as a line under the field.
void gfxSetCursorBrackets(int brackets) {
  (void)brackets;
}

void gfxClear(void) {
  SDL_Surface* target = trackerSurface();
  if (bgR == (backgroundBaseColor >> 16 & 255) &&
      bgG == (backgroundBaseColor >> 8 & 255) && bgB == (backgroundBaseColor & 255)) {
    SDL_Rect rect = {0, 0, (Uint16)target->w, (Uint16)target->h};
    drawBackgroundRect(rect);
  } else SDL_FillRect(target, NULL, bgColor);
  isDirty = 1;
}

void gfxPoint(int x, int y, uint32_t color) {
#ifdef MIYOOPORTS_BUILD
  ((Uint32 *)offscreenSurface->pixels)[y * offscreenSurface->w + x] = color;
#else
  ((Uint32 *)sdlScreen->pixels)[y * sdlScreen->w + x] = color;
#endif
  isDirty = 1;
}

void gfxClearRect(int x, int y, int w, int h) {
  SDL_Rect rect = { CHAR_X(x), CHAR_Y(y), CHAR_X(w), h * fontH };
  if (bgR == (backgroundBaseColor >> 16 & 255) &&
      bgG == (backgroundBaseColor >> 8 & 255) && bgB == (backgroundBaseColor & 255))
    drawBackgroundRect(rect);
  else SDL_FillRect(trackerSurface(), &rect, bgColor);
  isDirty = 1;
}

void gfxPrint(int x, int y, const char* text) {
  if (text == NULL) return;

  int cx = CHAR_X(x);
  int cy = CHAR_Y(y);
  int len = (int)strlen(text);

  // Draw background rectangles first
  for (int i = 0; i < len; i++) {
    if (text[i] == '\r' && text[i + 1] == '\n') {
      i++;
      cx = CHAR_X(x);
      cy += fontH;
      continue;
    }
    SDL_Rect bgRect = {cx, cy, fontW * 8, fontH};
    if (bgR == (backgroundBaseColor >> 16 & 255) &&
        bgG == (backgroundBaseColor >> 8 & 255) && bgB == (backgroundBaseColor & 255))
      drawBackgroundRect(bgRect);
    else SDL_FillRect(trackerSurface(), &bgRect, bgColor);
    cx += fontW * 8;
    if (cx > WINDOW_WIDTH) {
      cx = CHAR_X(x);
      cy += fontH;
    }
  }

  // Draw characters
  cx = CHAR_X(x);
  cy = CHAR_Y(y);

  for (int i = 0; i < len; i++) {
    uint8_t C = text[i];
    if (C == '\r' && text[i + 1] == '\n') {
      i++;
      cx = CHAR_X(x);
      cy += fontH;
      if (cy > WINDOW_HEIGHT) {
        cy = CHAR_Y(y);
      }
      continue;
    }

    if (C >= 32 && C <= 126) {
      SDL_Color colors[2] = {
        {0, 0, 0, 0},  // Index 0: transparent
        {fgR, fgG, fgB, 255}  // Index 1: foreground
      };
      SDL_SetColors(charSurfaces[C - 32], colors, 0, 2);

      SDL_Rect dstRect = {cx, cy, fontW * 8, fontH};
#ifdef MIYOOPORTS_BUILD
      SDL_BlitSurface(charSurfaces[C - 32], NULL, offscreenSurface, &dstRect);
#else
      SDL_BlitSurface(charSurfaces[C - 32], NULL, sdlScreen, &dstRect);
#endif
    }

    cx += fontW * 8;
    if (cx > WINDOW_WIDTH) {
      cx = CHAR_X(x);
      cy += fontH;
    }
  }
  isDirty = 1;
}

void gfxPrintf(int x, int y, const char* format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(printBuffer, 256, format, args);
  va_end(args);
  gfxPrint(x, y, printBuffer);
}

static int lastCursorX, lastCursorY, lastCursorW;

void gfxGetCursor(int* x, int* y, int* w) {
  *x = lastCursorX;
  *y = lastCursorY;
  *w = lastCursorW;
}

void gfxCursor(int x, int y, int w) {
  lastCursorX = x;
  lastCursorY = y;
  lastCursorW = w;
  SDL_Rect rect = { CHAR_X(x), CHAR_Y(y) + fontH - 1, CHAR_X(w), 1 };
#ifdef MIYOOPORTS_BUILD
  SDL_FillRect(offscreenSurface, &rect, cursorColor);
#else
  SDL_FillRect(sdlScreen, &rect, cursorColor);
#endif
  isDirty = 1;
}

void gfxRect(int x, int y, int w, int h) {
  int cx = CHAR_X(x);
  int cy = CHAR_Y(y);
  int cw = CHAR_X(w);
  int ch = h * fontH;

  SDL_Rect rects[4] = {
    {cx, cy, cw, 1},           // top
    {cx, cy + ch - 1, cw, 1}, // bottom
    {cx, cy, 1, ch},          // left
    {cx + cw - 1, cy, 1, ch}  // right
  };

#ifdef MIYOOPORTS_BUILD
  for (int i = 0; i < 4; i++) {
    SDL_FillRect(offscreenSurface, &rects[i], fgColor);
  }
#else
  for (int i = 0; i < 4; i++) {
    SDL_FillRect(sdlScreen, &rects[i], fgColor);
  }
#endif
  isDirty = 1;
}

void gfxUpdateScreen(void) {
#ifdef MIYOOPORTS_BUILD
  if (isDirty && offscreenSurface) {
    SDL_LockSurface(offscreenSurface);
    SDL_LockSurface(sdlScreen);
    int w = offscreenSurface->w;
    int h = offscreenSurface->h;
    Uint32* src = (Uint32*)offscreenSurface->pixels;
    Uint32* dst = (Uint32*)sdlScreen->pixels;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        dst[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
      }
    }
    SDL_UnlockSurface(offscreenSurface);
    SDL_UnlockSurface(sdlScreen);
    SDL_Flip(sdlScreen);
    isDirty = 0;
  }
#else
  if (isDirty) {
    SDL_UpdateRect(sdlScreen, 0, 0, 0, 0);
    isDirty = 0;
  }
#endif
}

static void drawAlphaBitmap(const uint8_t* data, int width, int height, int cx, int cy) {
  SDL_Surface* target = trackerSurface();
  const bool backdrop = bgR == (backgroundBaseColor >> 16 & 255) &&
    bgG == (backgroundBaseColor >> 8 & 255) && bgB == (backgroundBaseColor & 255);
  if (SDL_MUSTLOCK(target) && SDL_LockSurface(target) != 0) return;
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    int px = cx + x, py = cy + y;
    if (px < 0 || px >= target->w || py < 0 || py >= target->h) continue;
    uint8_t alpha = data[y * width + x];
    if (backdrop && !alpha) continue;
    Uint32* destination = (Uint32*)((Uint8*)target->pixels + py * target->pitch + px * 4);
    Uint8 underR = bgR, underG = bgG, underB = bgB;
    if (backdrop) SDL_GetRGB(*destination, target->format, &underR, &underG, &underB);
    Uint8 r = underR + ((fgR - underR) * alpha) / 255;
    Uint8 g = underG + ((fgG - underG) * alpha) / 255;
    Uint8 b = underB + ((fgB - underB) * alpha) / 255;
    *destination = SDL_MapRGB(target->format, r, g, b);
  }
  if (SDL_MUSTLOCK(target)) SDL_UnlockSurface(target);
}

void gfxDrawCharBitmap(uint8_t* bitmap, int col, int row) {
  drawAlphaBitmap(bitmap, fontW * 8, fontH, CHAR_X(col), CHAR_Y(row));
  isDirty = 1;
}

Bitmap* gfxBitmapCreate(int widthChars, int heightChars) {
  Bitmap* bitmap = (Bitmap*)malloc(sizeof(Bitmap));
  if (!bitmap) return NULL;

  int charW = fontW * 8;
  bitmap->widthChars = widthChars;
  bitmap->heightChars = heightChars;
  bitmap->widthPixels = widthChars * charW;
  bitmap->heightPixels = heightChars * fontH;

  int dataSize = bitmap->widthPixels * bitmap->heightPixels;
  bitmap->data = (uint8_t*)malloc(dataSize);
  if (!bitmap->data) {
    free(bitmap);
    return NULL;
  }
  memset(bitmap->data, 0, dataSize);
  bitmap->userdata = NULL;  // SDL1.2 doesn't use textures

  return bitmap;
}

void gfxBitmapClear(Bitmap* bitmap) {
  memset(bitmap->data, 0, bitmap->widthPixels * bitmap->heightPixels);
}

void gfxBitmapFree(Bitmap* bitmap) {
  if (!bitmap) return;
  if (bitmap->data) {
    free(bitmap->data);
  }
  free(bitmap);
}

void gfxDrawBitmap(Bitmap* bitmap, int col, int row) {
  if (!bitmap || !bitmap->data) return;
  drawAlphaBitmap(bitmap->data, bitmap->widthPixels, bitmap->heightPixels,
    CHAR_X(col), CHAR_Y(row));
  isDirty = 1;
}

int gfxGetCharWidth(void) {
  return fontW * 8;
}

int gfxGetCharHeight(void) {
  return fontH;
}

void gfxReloadFont(void) {
  for (int i = 0; i < 95; i++) {
    if (charSurfaces[i]) {
      SDL_FreeSurface(charSurfaces[i]);
      charSurfaces[i] = NULL;
    }
  }

  currentResolution = fontSelectResolution(fontGetCurrent(), WINDOW_WIDTH, WINDOW_HEIGHT);
  if (!currentResolution) {
    currentResolution = &fontGetDefault()->resolutions[1];
  }

  createCharSurfaces();
  isDirty = 1;
}

void gfxDrawHUD(void) {}

void gfxSetButtonPressed(int buttonIndex, int pressed) {
  (void)buttonIndex;
  (void)pressed;
}

const char* gfxGetRendererType(void) {
  return "SDL 1.2 software";
}

GfxImage* gfxImageLoadBMP(const char* path) {
  SDL_Surface* loaded = SDL_LoadBMP(path);
  if (!loaded) {
    printf("Failed to load title image %s: %s\n", path, SDL_GetError());
    return NULL;
  }

  SDL_Surface* imageSurface = SDL_DisplayFormatAlpha(loaded);
  SDL_FreeSurface(loaded);
  if (!imageSurface) {
    printf("Failed to convert title image %s: %s\n", path, SDL_GetError());
    return NULL;
  }
  if (imageSurface->format->BytesPerPixel != 4) {
    printf("Unsupported pixel format for title image %s\n", path);
    SDL_FreeSurface(imageSurface);
    return NULL;
  }

  if (SDL_LockSurface(imageSurface) != 0) {
    printf("Failed to lock title image %s: %s\n", path, SDL_GetError());
    SDL_FreeSurface(imageSurface);
    return NULL;
  }
  for (int y = 0; y < imageSurface->h; y++) {
    Uint32* row = (Uint32*)((Uint8*)imageSurface->pixels + y * imageSurface->pitch);
    for (int x = 0; x < imageSurface->w; x++) {
      Uint8 r, g, b, a;
      SDL_GetRGBA(row[x], imageSurface->format, &r, &g, &b, &a);
      if (r > 100 && b > 100 && r > g * 1.6f && b > g * 1.6f) a = 0;
      row[x] = SDL_MapRGBA(imageSurface->format, r, g, b, a);
    }
  }
  SDL_UnlockSurface(imageSurface);
  if (SDL_SetAlpha(imageSurface, SDL_SRCALPHA, 255) != 0) {
    printf("Failed to enable title image transparency for %s: %s\n", path, SDL_GetError());
    SDL_FreeSurface(imageSurface);
    return NULL;
  }

  GfxImage* image = (GfxImage*)malloc(sizeof(GfxImage));
  if (!image) {
    SDL_FreeSurface(imageSurface);
    return NULL;
  }
  image->surface = imageSurface;
  image->width = imageSurface->w;
  image->height = imageSurface->h;
  return image;
}

GfxImage* gfxImageCreateARGB(int width, int height) {
  SDL_Surface* surface = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32,
    0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
  if (!surface) return NULL;
  GfxImage* image = (GfxImage*)malloc(sizeof(GfxImage));
  if (!image) { SDL_FreeSurface(surface); return NULL; }
  SDL_SetAlpha(surface, 0, 255);
  image->surface = surface; image->width = width; image->height = height;
  return image;
}

int gfxImageUpdateARGB(GfxImage* image, const uint32_t* pixels) {
  if (!image || !pixels || SDL_LockSurface(image->surface) != 0) return 0;
  for (int y = 0; y < image->height; ++y)
    memcpy((uint8_t*)image->surface->pixels + y * image->surface->pitch,
      pixels + y * image->width, image->width * 4);
  SDL_UnlockSurface(image->surface);
  return 1;
}

void gfxImageFree(GfxImage* image) {
  if (!image) return;
  if (image->surface) SDL_FreeSurface(image->surface);
  free(image);
}

int gfxImageWidth(const GfxImage* image) {
  return image ? image->width : 0;
}

int gfxImageHeight(const GfxImage* image) {
  return image ? image->height : 0;
}

void gfxImageDrawCrop(const GfxImage* image, int sourceX, int sourceY,
  int sourceW, int sourceH, int destinationX, int destinationY) {
  if (!image || !image->surface || !titleSurface ||
      sourceX < 0 || sourceY < 0 || sourceW <= 0 || sourceH <= 0 ||
      sourceX + sourceW > image->width || sourceY + sourceH > image->height) return;

  SDL_Rect source = {sourceX, sourceY, sourceW, sourceH};
  SDL_Rect destination = {destinationX, destinationY, sourceW, sourceH};
  if (SDL_BlitSurface(image->surface, &source, titleSurface, &destination) != 0) {
    printf("Failed to draw title image: %s\n", SDL_GetError());
  }
}

void gfxTitleBegin(void) {
  if (!titleSurface) {
    titleSurface = SDL_CreateRGBSurface(SDL_SWSURFACE, 256, 224, 32,
      sdlScreen->format->Rmask, sdlScreen->format->Gmask,
      sdlScreen->format->Bmask, sdlScreen->format->Amask);
    if (!titleSurface) {
      printf("Failed to create title surface: %s\n", SDL_GetError());
      return;
    }
  }
  SDL_FillRect(titleSurface, NULL, SDL_MapRGB(titleSurface->format, 5, 12, 31));
}

void gfxTitleFadeBlack(uint8_t alpha) {
  if (!titleSurface || alpha == 0) return;
  if (SDL_LockSurface(titleSurface) != 0) {
    printf("Failed to lock title surface: %s\n", SDL_GetError());
    return;
  }
  for (int y = 0; y < titleSurface->h; y++) {
    Uint32* row = (Uint32*)((Uint8*)titleSurface->pixels + y * titleSurface->pitch);
    for (int x = 0; x < titleSurface->w; x++) {
      Uint8 r, g, b;
      SDL_GetRGB(row[x], titleSurface->format, &r, &g, &b);
      r = (Uint8)(r * (255 - alpha) / 255);
      g = (Uint8)(g * (255 - alpha) / 255);
      b = (Uint8)(b * (255 - alpha) / 255);
      row[x] = SDL_MapRGB(titleSurface->format, r, g, b);
    }
  }
  SDL_UnlockSurface(titleSurface);
}

void gfxTitlePresent(void) {
  if (!titleSurface) return;
#ifdef MIYOOPORTS_BUILD
  SDL_Surface* target = offscreenSurface;
#else
  SDL_Surface* target = sdlScreen;
#endif
  if (!target) return;

  SDL_Rect destination = {0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
  if (SDL_SoftStretch(titleSurface, NULL, target, &destination) != 0) {
    printf("Failed to present title screen: %s\n", SDL_GetError());
    return;
  }
  isDirty = 1;
}

void gfxTitleEnd(void) {
  if (titleSurface) {
    SDL_FreeSurface(titleSurface);
    titleSurface = NULL;
  }
}

void gfxTitlePrint(int x, int y, const char* text) {
  if (!text || !titleSurface || !currentResolution || !currentResolution->data) return;
  const int sourceW = currentResolution->charWidth;
  const int sourceH = currentResolution->charHeight;
  const int sourceBytes = (sourceW + 7) / 8;
  Uint32 color = SDL_MapRGB(titleSurface->format, fgR, fgG, fgB);

  if (SDL_LockSurface(titleSurface) != 0) {
    printf("Failed to lock title surface: %s\n", SDL_GetError());
    return;
  }
  for (int i = 0; text[i]; i++) {
    const uint8_t c = (uint8_t)text[i];
    if (c < 32 || c > 126) continue;
    const uint8_t* glyph = currentResolution->data +
      (c - 32) * sourceBytes * sourceH;
    for (int dy = 0; dy < 12; dy++) {
      const int y0 = dy * sourceH / 12;
      const int y1 = ((dy + 1) * sourceH - 1) / 12;
      for (int dx = 0; dx < 8; dx++) {
        const int x0 = dx * sourceW / 8;
        const int x1 = ((dx + 1) * sourceW - 1) / 8;
        int set = 0;
        for (int sy = y0; sy <= y1 && !set; sy++) {
          for (int sx = x0; sx <= x1; sx++) {
            if (glyph[sy * sourceBytes + sx / 8] & (0x80 >> (sx & 7))) {
              set = 1;
              break;
            }
          }
        }
        if (!set) continue;
        const int px = x * 8 + i * 8 + dx;
        const int py = y * 12 + dy;
        if (px >= 0 && px < titleSurface->w && py >= 0 && py < titleSurface->h) {
          Uint32* row = (Uint32*)((Uint8*)titleSurface->pixels + py * titleSurface->pitch);
          row[px] = color;
        }
      }
    }
  }
  SDL_UnlockSurface(titleSurface);
}
