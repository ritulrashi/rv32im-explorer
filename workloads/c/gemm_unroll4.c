/* GEMM, naive i-j-k with the k loop unrolled x4 by hand. N % 4 == 0. */
#include "roi.h"

extern int32_t mat_a[N][N];
extern int32_t mat_b[N][N];
extern int32_t mat_c[N][N];
extern const uint32_t gemm_expected;

__attribute__((noinline)) void gemm(int32_t (*a)[N], int32_t (*b)[N], int32_t (*c)[N]) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      int32_t acc = 0;
      for (int k = 0; k < N; k += 4) {
        acc += a[i][k] * b[k][j];
        acc += a[i][k + 1] * b[k + 1][j];
        acc += a[i][k + 2] * b[k + 2][j];
        acc += a[i][k + 3] * b[k + 3][j];
      }
      c[i][j] = acc;
    }
  }
}

int main(void) {
  ROI_BEGIN();
  gemm(mat_a, mat_b, mat_c);
  ROI_END();
  return rvx_checksum(&mat_c[0][0], N * N) == gemm_expected ? 1 : 3;
}
