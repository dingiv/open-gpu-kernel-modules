/*
 * P3 probe framework — tagged, throttled, compile-time-switchable probes
 * for the METHOD3 dynamic-BAR1 P2P work (dual-3080 alignment campaign).
 *
 * COMPILE-TIME master switch:
 *   make ... METHOD3_PROBES=1     -> probes compiled in (RM + kernel interface)
 *   default                       -> P3_PROBE() expands to nothing, zero cost
 *
 * RUNTIME control (only when compiled in), /sys/module/nvidia/parameters/:
 *   nv_p3_tags   (ullong) bitmask of enabled tag groups, default all
 *   nv_p3_first  (uint)   print the first N hits per callsite,  default 16
 *   nv_p3_every  (uint)   then print every Mth hit,             default 4096
 *                         (0 = after the first N, never again)
 *
 * TAG GROUPS:
 *   P3_TAG_MAP    window map/unmap lifecycle            (cold)
 *   P3_TAG_PTE    PTE-encode entry per mapping          (warm)
 *   P3_TAG_PAGE   PTE encode per page                   (VERY hot, throttle!)
 *   P3_TAG_APERT  aperture grants + VA placement floors (align3 decision pt)
 *   P3_TAG_PEERQ  peer phys-addr queries / DupMemory    (choke point)
 *   P3_TAG_HOT    data-plane CPU map churn              (hot, throttled)
 *
 * All lines print as:  P3[<function>] <message>
 */
#ifndef _P3_PROBE_H_
#define _P3_PROBE_H_

#if defined(NV_P3_PROBES)

#define P3_TAG_MAP    (0x1ULL << 0)
#define P3_TAG_PTE    (0x1ULL << 1)
#define P3_TAG_PAGE   (0x1ULL << 2)
#define P3_TAG_APERT  (0x1ULL << 3)
#define P3_TAG_PEERQ  (0x1ULL << 4)
#define P3_TAG_HOT    (0x1ULL << 5)
#define P3_TAG_ALL    (0xFFFFULL)

/*
 * Implemented in kernel-open/nvidia/nv.c (same nvidia.ko): checks the
 * runtime tag mask and the per-callsite first-N/every-M throttle.
 */
NvBool nv_p3_ok(NvU64 tag, NvU32 *pCount);
int nv_p3_pid(void);

#define P3_PROBE(tag, fmt, ...)                                              \
    do                                                                       \
    {                                                                        \
        static NvU32 p3Cnt_;                                                 \
        if (nv_p3_ok((tag), &p3Cnt_))                                        \
        {                                                                    \
            NV_PRINTF(LEVEL_ERROR, "P3[%s p%d] " fmt, __func__,              \
                      nv_p3_pid(), ##__VA_ARGS__);                           \
        }                                                                    \
    } while (0)

#else  /* !NV_P3_PROBES */

#define P3_TAG_MAP    0
#define P3_TAG_PTE    0
#define P3_TAG_PAGE   0
#define P3_TAG_APERT  0
#define P3_TAG_PEERQ  0
#define P3_TAG_HOT    0
#define P3_TAG_ALL    0

#define P3_PROBE(tag, fmt, ...)   do { } while (0)

#endif /* NV_P3_PROBES */

#endif /* _P3_PROBE_H_ */
