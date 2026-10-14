/* Region-of-interest markers and result check for C workloads. */
#ifndef RVX_ROI_H
#define RVX_ROI_H

#include <stdint.h>

extern volatile int32_t rvx_roi;

/* The volatile store is the marker; the empty asm with a "memory" clobber
 * stops the compiler from moving kernel loads/stores across it. Kernels are
 * also noinline functions, so their bodies cannot migrate out of the ROI. */
#define ROI_BEGIN()                    \
  do {                                 \
    __asm__ volatile("" ::: "memory"); \
    rvx_roi = 1;                       \
    __asm__ volatile("" ::: "memory"); \
  } while (0)
#define ROI_END()                      \
  do {                                 \
    __asm__ volatile("" ::: "memory"); \
    rvx_roi = 2;                       \
    __asm__ volatile("" ::: "memory"); \
  } while (0)

/* Same hash as workloads/common/check.S and gen_data.py. */
static inline uint32_t rvx_checksum(const int32_t* p, int n) {
  uint32_t h = 0;
  for (int i = 0; i < n; ++i) h = h * 31u + (uint32_t)p[i];
  return h;
}

#endif
