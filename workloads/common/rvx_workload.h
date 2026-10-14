/* Macros for hand-written assembly workloads (run through the C preprocessor).
 * ROI_BEGIN / ROI_END clobber t5 and t6. */
#ifndef RVX_WORKLOAD_H
#define RVX_WORKLOAD_H

#define ROI_BEGIN \
  li t6, 1;       \
  la t5, rvx_roi; \
  sw t6, 0(t5)

#define ROI_END   \
  li t6, 2;       \
  la t5, rvx_roi; \
  sw t6, 0(t5)

#define PASS      \
  la t0, tohost;  \
  li t1, 1;       \
  sw t1, 0(t0);   \
  j .

#define FAIL      \
  la t0, tohost;  \
  li t1, 3;       \
  sw t1, 0(t0);   \
  j .

#endif
