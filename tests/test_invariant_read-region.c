#include <check.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

/*
 * Simulate the vulnerable allocation pattern from misc/read-region.c:24
 * The invariant: if w * h * 4 overflows, the allocation must be rejected
 * (return NULL or fail), never silently produce an undersized buffer.
 */

/* Safe multiplication check: returns 0 on overflow, 1 on success */
static int safe_multiply_check(uint64_t w, uint64_t h, uint64_t *result)
{
    /* Check for overflow in w * h */
    if (w != 0 && h > UINT64_MAX / w) {
        return 0; /* overflow */
    }
    uint64_t wh = w * h;

    /* Check for overflow in wh * 4 */
    if (wh > UINT64_MAX / 4) {
        return 0; /* overflow */
    }
    *result = wh * 4;
    return 1;
}

/*
 * Hardened allocation that mirrors what the fixed code SHOULD do.
 * Returns NULL if dimensions would overflow or if malloc fails.
 */
static uint32_t *safe_alloc_region(uint64_t w, uint64_t h)
{
    uint64_t size = 0;

    if (!safe_multiply_check(w, h, &size)) {
        /* Overflow detected — must reject */
        return NULL;
    }

    if (size == 0) {
        return NULL;
    }

    /* SIZE_MAX guard for platforms where size_t < uint64_t */
    if (size > (uint64_t)SIZE_MAX) {
        return NULL;
    }

    return (uint32_t *)malloc((size_t)size);
}

/* ---------- test cases ---------- */

START_TEST(test_overflow_dimensions_rejected)
{
    /*
     * Invariant: Buffer reads never exceed the declared length.
     * Specifically: oversized/crafted dimension values that would cause
     * w * h * 4 to overflow must be REJECTED (NULL returned), never
     * silently produce an undersized buffer that later overflows.
     */

    typedef struct {
        uint64_t w;
        uint64_t h;
        const char *description;
    } DimPayload;

    const DimPayload payloads[] = {
        /* Classic 32-bit overflow: 0x10000 * 0x10000 = 0x100000000 wraps to 0 on 32-bit */
        { 0x10000ULL,          0x10000ULL,          "32-bit wrap: 64k x 64k"              },
        /* Values that overflow 64-bit when multiplied by 4 */
        { 0x4000000000000000ULL, 2ULL,              "64-bit overflow: huge w, h=2"         },
        { 2ULL,                0x4000000000000000ULL,"64-bit overflow: w=2, huge h"         },
        /* UINT32_MAX dimensions */
        { 0xFFFFFFFFULL,       0xFFFFFFFFULL,        "UINT32_MAX x UINT32_MAX"              },
        /* Slightly above 32-bit boundary */
        { 0x100000001ULL,      0x100000001ULL,        "just above 32-bit boundary"           },
        /* Large but not quite overflowing — should succeed if memory available */
        /* (we only check that NULL is returned on overflow, not on OOM)        */
        { 0xFFFFFFFFFFFFFFFFULL, 1ULL,               "UINT64_MAX width"                     },
        { 1ULL,                0xFFFFFFFFFFFFFFFFULL, "UINT64_MAX height"                    },
        { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,"UINT64_MAX x UINT64_MAX"             },
        /* Overflow specifically in the *4 step */
        { 0x2000000000000000ULL, 2ULL,               "overflow in *4 step"                  },
        { 0x8000000000000000ULL, 1ULL,               "overflow in *4: w=2^63"               },
        /* Attacker-style crafted values that wrap to small sizes on 32-bit */
        { 0x80000001ULL,       0x80000001ULL,         "crafted wrap to small size"           },
        { 0xFFFF0001ULL,       0xFFFF0001ULL,         "crafted near-UINT32_MAX"              },
        /* Zero dimensions */
        { 0ULL,                1000ULL,               "zero width"                           },
        { 1000ULL,             0ULL,                  "zero height"                          },
        { 0ULL,                0ULL,                  "zero width and height"                },
    };

    int num_payloads = (int)(sizeof(payloads) / sizeof(payloads[0]));

    for (int i = 0; i < num_payloads; i++) {
        uint64_t w = payloads[i].w;
        uint64_t h = payloads[i].h;

        uint64_t computed_size = 0;
        int overflows = !safe_multiply_check(w, h, &computed_size);

        uint32_t *buf = safe_alloc_region(w, h);

        if (overflows) {
            /*
             * INVARIANT: if the multiplication overflows, the allocation
             * MUST be rejected (NULL). An undersized buffer must never
             * be returned to the caller.
             */
            ck_assert_msg(buf == NULL,
                "SECURITY VIOLATION [%s]: overflow dimensions (w=%llu, h=%llu) "
                "produced a non-NULL allocation instead of being rejected. "
                "This would result in an undersized buffer and heap overflow.",
                payloads[i].description,
                (unsigned long long)w,
                (unsigned long long)h);
        } else if (w == 0 || h == 0) {
            /* Zero-size: must also be rejected */
            ck_assert_msg(buf == NULL,
                "SECURITY VIOLATION [%s]: zero dimensions (w=%llu, h=%llu) "
                "should be rejected.",
                payloads[i].description,
                (unsigned long long)w,
                (unsigned long long)h);
        } else {
            /*
             * Non-overflowing case: if allocation succeeded, verify the
             * buffer is large enough to hold w*h pixels (4 bytes each).
             * We do a boundary write to the last valid byte to confirm.
             */
            if (buf != NULL) {
                uint64_t num_pixels = w * h;
                /* Write to first and last pixel to check bounds */
                buf[0] = 0xDEADBEEFU;
                buf[num_pixels - 1] = 0xCAFEBABEU;
                ck_assert_uint_eq(buf[0], 0xDEADBEEFU);
                ck_assert_uint_eq(buf[num_pixels - 1], 0xCAFEBABEU);
                free(buf);
            }
            /* NULL here just means OOM, which is acceptable */
        }
    }
}
END_TEST

START_TEST(test_32bit_overflow_simulation)
{
    /*
     * Invariant: Even when dimensions fit in uint32_t individually,
     * their product must be checked for overflow before allocation.
     *
     * Simulate the exact vulnerable pattern: uint32_t w * h * 4
     * and verify that the safe version catches what the vulnerable one misses.
     */

    typedef struct {
        uint32_t w;
        uint32_t h;
    } Dim32;

    const Dim32 payloads[] = {
        { 0xFFFF,     0xFFFF     },  /* 65535 * 65535 * 4 overflows 32-bit */
        { 0x10000,    0x10000    },  /* 65536 * 65536 * 4 = 0 on 32-bit    */
        { 0x8000,     0x10000    },  /* 32768 * 65536 * 4 overflows 32-bit  */
        { 0xFFFFFFFF, 1          },  /* max * 1 * 4 overflows               */
        { 1,          0xFFFFFFFF },  /* 1 * max * 4 overflows               */
        { 0x80000000, 2          },  /* 2^31 * 2 * 4 overflows              */
        { 0x40000000, 4          },  /* 2^30 * 4 * 4 overflows              */
        { 0x20000000, 8          },  /* 2^29 * 8 * 4 overflows              */
        { 0x10000000, 16         },  /* 2^28 * 16 * 4 overflows             */
        { 0xFFFF,     0x10000    },  /* near-boundary                       */
    };

    int num_payloads = (int)(sizeof(payloads) / sizeof(payloads[0]));

    for (int i = 0; i < num_payloads; i++) {
        uint32_t w32 = payloads[i].w;
        uint32_t h32 = payloads[i].h;

        /* Vulnerable pattern: no overflow check */
        uint32_t vulnerable_size = w32 * h32 * 4;  /* may overflow silently */

        /* Safe pattern: use 64-bit arithmetic to detect overflow */
        uint64_t safe_size = (uint64_t)w32 * (uint64_t)h32 * 4ULL;

        /*
         * INVARIANT: if the 64-bit size differs from the 32-bit size,
         * overflow occurred. The safe allocator must return NULL.
         * The vulnerable allocator would return a dangerously small buffer.
         */
        if (safe_size != (uint64_t)vulnerable_size || safe_size > (uint64_t)SIZE_MAX) {
            /* Overflow detected in 32-bit arithmetic */
            uint32_t *safe_buf = safe_alloc_region((uint64_t)w32, (uint64_t)h32);
            ck_assert_msg(safe_buf == NULL,
                "SECURITY VIOLATION: overflow in 32-bit w(%u)*h(%u)*4 "
                "must be caught and rejected. safe_size=%llu, vulnerable_size=%u",
                w32, h32,
                (unsigned long long)safe_size,
                vulnerable_size);

            /*
             * Also assert that the vulnerable size is SMALLER than the safe size,
             * confirming the overflow would produce an undersized buffer.
             */
            if (safe_size <= (uint64_t)SIZE_MAX) {
                ck_assert_msg((uint64_t)vulnerable_size < safe_size,
                    "Expected vulnerable_size(%u) < safe_size(%llu) due to overflow",
                    vulnerable_size, (unsigned long long)safe_size);
            }
        }
    }
}
END_TEST

START_TEST(test_valid_small_dimensions_succeed)
{
    /*
     * Invariant: Valid, small dimensions must produce a correctly-sized
     * buffer. The security check must not reject legitimate inputs.
     */

    typedef struct {
        uint64_t w;
        uint64_t h;
        const char *description;
    } ValidDim;

    const ValidDim payloads[] = {
        { 1,    1,    "1x1 pixel"       },
        { 100,  100,  "100x100"         },
        { 640,  480,  "VGA"             },
        { 1920, 1080, "Full HD"         },
        { 256,  256,  "256x256"         },
        { 4096, 4096, "4096x4096"       },
    };

    int num_payloads = (int)(sizeof(payloads) / sizeof(payloads[0]));

    for (int i = 0; i < num_payloads; i++) {
        uint64_t w = payloads[i].w;
        uint64_t h = payloads[i].h;
        uint64_t expected_size = w * h * 4;

        uint32_t *buf = safe_alloc_region(w, h);

        if (buf != NULL) {
            /*
             * INVARIANT: the allocated buffer must be large enough.
             * Write to first and last element to verify no overflow.
             */
            uint64_t num_pixels = w * h;
            buf[0] = 0x11111111U;
            buf[num_pixels - 1] = 0x22222222U;

            ck_assert_uint_eq(buf[0], 0x11111111U);
            ck_assert_uint_eq(buf[num_pixels - 1], 0x22222222U);

            /* Verify size is correct by checking we can address all pixels */
            for (uint64_t p = 0; p < num_pixels && p < 16; p++) {
                buf[p] = (uint32_t)p;
            }
            for (uint64_t p = 0; p < num_pixels && p < 16; p++) {
                ck_assert_uint_eq(buf[p], (uint32_t)p);
            }

            free(buf);
        }
        /* NULL means OOM on this system, which is acceptable for large dims */

        (void)expected_size;
    }
}
END_TEST

/* ---------- suite wiring ---------- */

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security_CWE120_BufferOverflow");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_overflow_dimensions_rejected);
    tcase_add_test(tc_core, test_32bit_overflow_simulation);
    tcase_add_test(tc_core, test_valid_small_dimensions_succeed);

    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}