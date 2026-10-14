/* dot product, unrolled x4 with four accumulators. N % 4 == 0. */
#include "roi.h"

extern int32_t vec_a[N];
extern int32_t vec_b[N];
extern const int32_t dot_expected;

__attribute__((noinline)) int32_t dot(const int32_t* a, const int32_t* b) {
  int32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  for (int i = 0; i < N; i += 4) {
    s0 += a[i] * b[i];
    s1 += a[i + 1] * b[i + 1];
    s2 += a[i + 2] * b[i + 2];
    s3 += a[i + 3] * b[i + 3];
  }
  return (s0 + s1) + (s2 + s3);
}

int main(void) {
  ROI_BEGIN();
  const int32_t r = dot(vec_a, vec_b);
  ROI_END();
  return r == dot_expected ? 1 : 3;
}
