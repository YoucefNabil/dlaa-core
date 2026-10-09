#pragma once
// Small helpers for camera work: 4x4 row-major double matrices and jitter.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace dlaa_core {

// Gauss-Jordan with partial pivoting; m and out are rows of a 4x4 matrix.
inline bool Invert(const double* m, double* out) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
    if (std::fabs(a[pivot][c]) < 1e-12) return false;
    if (pivot != c)
      for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
    const double inv = 1.0 / a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c || a[r][c] == 0.0) continue;
      const double f = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
  return true;
}

// out = a * b (out may alias a or b).
inline void Multiply(const double* a, const double* b, double* out) {
  double t[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double s = 0.0;
      for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
      t[r * 4 + c] = s;
    }
  std::memcpy(out, t, sizeof(t));
}

inline float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  for (; index; index /= base) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(index % base);
  }
  return r;
}

// Jitter of frame `seq` in pixels, in [-0.5, 0.5): Halton(2, 3) over a
// 32-frame cycle. Pixel y points down; NDC y points up (negate for NDC).
inline void HaltonJitter(uint32_t seq, float* x, float* y) {
  const uint32_t n = seq % 32 + 1;
  *x = Halton(n, 2) - 0.5f;
  *y = Halton(n, 3) - 0.5f;
}

}  // namespace dlaa_core
