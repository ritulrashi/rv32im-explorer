/* dot product, naive. Built with -DN=<elements>. */
#include "roi.h"

extern int32_t vec_a[N];
extern int32_t vec_b[N];
extern const int32_t dot_expected;

__attribute__((noinline)) int32_t dot(const int32_t* a, const int32_t* b) {
  int32_t s = 0;
  for (int i = 0; i < N; ++i) s += a[i] * b[i];
  return s;
}

int main(void) {
  ROI_BEGIN();
  const int32_t r = dot(vec_a, vec_b);
  ROI_END();
  return r == dot_expected ? 1 : 3;
}
