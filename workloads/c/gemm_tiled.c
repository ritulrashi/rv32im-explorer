/* GEMM, tiled with T x T blocks (T = 8), i-j-k inside each tile.
 * Same loop structure as workloads/asm/gemm_tiled.S. N % 8 == 0. */
#include "roi.h"

#define T 8

extern int32_t mat_a[N][N];
extern int32_t mat_b[N][N];
extern int32_t mat_c[N][N];
extern const uint32_t gemm_expected;

__attribute__((noinline)) void gemm(int32_t (*a)[N], int32_t (*b)[N], int32_t (*c)[N]) {
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
  return rvx_checksum(&mat_c[0][0], N * N) == gemm_expected ? 1 : 3;
}
