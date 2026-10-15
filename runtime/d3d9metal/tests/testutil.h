// Our own helpers shared by the conformance test programs: a reference DXT1/3/5 decoder, a deterministic
// random generator, and a BMP writer (converted to PNG with `sips` by the build script).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline uint32_t tu_rand(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static inline void tu_565(uint16_t c, int rgb[3]) {
  int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  rgb[0] = (r << 3) | (r >> 2); rgb[1] = (g << 2) | (g >> 4); rgb[2] = (b << 3) | (b >> 2);
}

// Decode one 4x4 color block (DXT1 semantics when dxt1 != 0) into out[16][4] RGBA.
static inline void tu_color_block(const uint8_t *b, int dxt1, uint8_t out[16][4]) {
  uint16_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
  int p[4][4], a[4] = {255, 255, 255, 255};
  tu_565(c0, p[0]); tu_565(c1, p[1]);
  for (int k = 0; k < 3; k++) {
    if (!dxt1 || c0 > c1) {
      p[2][k] = (2 * p[0][k] + p[1][k]) / 3; p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
    } else {
      p[2][k] = (p[0][k] + p[1][k]) / 2; p[3][k] = 0;
    }
  }
  if (dxt1 && c0 <= c1) a[3] = 0;
  uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
  for (int i = 0; i < 16; i++) {
    int s = (idx >> (2 * i)) & 3;
    out[i][0] = p[s][0]; out[i][1] = p[s][1]; out[i][2] = p[s][2]; out[i][3] = a[s];
  }
}

// fmt: 1 = DXT1/BC1, 3 = DXT3/BC2, 5 = DXT5/BC3. Decodes a w x h image (multiples of 4) to RGBA8.
static inline void tu_decode_dxt(int fmt, const uint8_t *src, int w, int h, uint8_t *rgba) {
  int bs = fmt == 1 ? 8 : 16;
  for (int by = 0; by < h / 4; by++)
    for (int bx = 0; bx < w / 4; bx++) {
      const uint8_t *b = src + (by * (w / 4) + bx) * bs;
      uint8_t px[16][4];
      tu_color_block(fmt == 1 ? b : b + 8, fmt == 1, px);
      if (fmt == 3) {
        for (int i = 0; i < 16; i++) { int v = (b[i / 2] >> (4 * (i & 1))) & 15; px[i][3] = v * 17; }
      } else if (fmt == 5) {
        int a0 = b[0], a1 = b[1], al[8];
        al[0] = a0; al[1] = a1;
        if (a0 > a1) { for (int k = 1; k < 7; k++) al[k + 1] = ((7 - k) * a0 + k * a1) / 7; }
        else { for (int k = 1; k < 5; k++) al[k + 1] = ((5 - k) * a0 + k * a1) / 5; al[6] = 0; al[7] = 255; }
        uint64_t bits = 0;
        for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
        for (int i = 0; i < 16; i++) px[i][3] = al[(bits >> (3 * i)) & 7];
      }
      for (int i = 0; i < 16; i++) {
        uint8_t *d = rgba + (((by * 4 + i / 4) * w) + bx * 4 + (i % 4)) * 4;
        memcpy(d, px[i], 4);
      }
    }
}

// Write a 24-bit BMP (alpha dropped, so screenshots stay opaque) from BGRA rows (top-down input).
// Returns 0 on success.
static inline int tu_write_bmp(const char *path, const uint8_t *bgra, int w, int h, int pitch) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, img = row * h, fsz = 54 + img;
  uint8_t hd[54] = {'B', 'M'};
  memcpy(hd + 2, &fsz, 4); uint32_t off = 54; memcpy(hd + 10, &off, 4);
  uint32_t hs = 40; memcpy(hd + 14, &hs, 4); int32_t ww = w, hh = -h; memcpy(hd + 18, &ww, 4); memcpy(hd + 22, &hh, 4);
  uint16_t planes = 1, bpp = 24; memcpy(hd + 26, &planes, 2); memcpy(hd + 28, &bpp, 2); memcpy(hd + 34, &img, 4);
  fwrite(hd, 1, 54, f);
  uint8_t line[3 * 8192 + 4] = {0};
  for (int y = 0; y < h; y++) {
    const uint8_t *src = bgra + (size_t)y * pitch;
    for (int x = 0; x < w && x < 8192; x++) { line[3 * x] = src[4 * x]; line[3 * x + 1] = src[4 * x + 1]; line[3 * x + 2] = src[4 * x + 2]; }
    fwrite(line, 1, row, f);
  }
  fclose(f);
  return 0;
}
