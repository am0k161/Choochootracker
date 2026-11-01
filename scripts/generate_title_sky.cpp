// Run from the repository root to refresh the title's baked first frame.
#include "../tracker/src/screens/title_night_sky.h"
#include <cstdio>
#include <memory>
int main() {
  auto sky = std::make_unique<TitleNightSky>();
  sky->render(0);
  FILE* file = std::fopen("tracker/packaging/common/title/night_sky.bmp", "wb");
  if (!file) return 1;
  const uint32_t size = 54 + 256 * 224 * 3;
  unsigned char header[54] = {'B', 'M'};
  auto put = [&](int offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) header[offset + i] = (value >> (8 * i)) & 255;
  };
  put(2, size); put(10, 54); put(14, 40); put(18, 256); put(22, 224);
  header[26] = 1; header[28] = 24;
  bool ok = std::fwrite(header, 1, 54, file) == 54;
  for (int y = 223; y >= 0; --y) for (int x = 0; x < 256; ++x) {
    uint32_t pixel = sky->pixels[y * 256 + x];
    unsigned char rgb[] = {static_cast<unsigned char>(pixel), static_cast<unsigned char>(pixel >> 8), static_cast<unsigned char>(pixel >> 16)};
    ok &= std::fwrite(rgb, 1, 3, file) == 3;
  }
  return std::fclose(file) || !ok;
}
