/* GEMM, tiled (T = 8), matrices stored with padded rows: row stride
 * LDA = N + PAD words. Investigation variant for cache set conflicts
 * (RESULTS.md). Built with -DN=<size> -DPAD=<words>. */
#include "roi.h"

#define T 8
#define LDA (N + PAD)

extern int32_t mat_a[N][LDA];
extern int32_t mat_b[N][LDA];
extern int32_t mat_c[N][LDA];
extern const uint32_t gemm_expected;

__attribute__((noinline)) void gemm(int32_t (*a)[LDA], int32_t (*b)[LDA], int32_t (*c)[LDA]) {
  for (int ii = 0; ii < N; ii += T) {
    for (int jj = 0; jj < N; jj += T) {
      for (int kk = 0; kk < N; kk += T) {
        for (int i = ii; i < ii + T; ++i) {
          for (int j = jj; j < jj + T; ++j) {
            int32_t acc = c[i][j];
            for (int k = kk; k < kk + T; ++k) acc += a[i][k] * b[k][j];
            c[i][j] = acc;
          }
        }
      }
    }
  }
}

int main(void) {
  ROI_BEGIN();
  gemm(mat_a, mat_b, mat_c);
  ROI_END();
  uint32_t h = 0;
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) h = h * 31u + (uint32_t)mat_c[i][j];
  return h == gemm_expected ? 1 : 3;
}
