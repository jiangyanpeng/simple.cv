#include "cv/c_api.h"
#include <limits.h>
#include <manager/data_manager.h>
#include <math.h>
namespace cv {
void resize_bilinear_c1_impl(const uint8_t* src,
                             uint32_t srcw,
                             uint32_t srch,
                             uint32_t srcstride,
                             uint8_t* dst,
                             uint32_t w,
                             uint32_t h,
                             uint32_t stride) {
    const int INTER_RESIZE_COEF_BITS  = 11;
    const int INTER_RESIZE_COEF_SCALE = 1 << INTER_RESIZE_COEF_BITS;
    //     const int ONE=INTER_RESIZE_COEF_SCALE;

    double scale_x = (double)srcw / w;
    double scale_y = (double)srch / h;

    int* buf = (int*)base::fast_malloc((w + h + w + h) * sizeof(int));

    int* xofs = buf;     // base::fast_malloc int[w];
    int* yofs = buf + w; // base::fast_malloc int[h];

    short* ialpha = (short*)(buf + w + h);     // base::fast_malloc short[w * 2];
    short* ibeta  = (short*)(buf + w + h + w); // base::fast_malloc short[h * 2];

    float fx;
    float fy;
    int sx;
    int sy;

#define SATURATE_CAST_SHORT(X) \
    (short)std::min(std::max((int)(X + (X >= 0.f ? 0.5f : -0.5f)), SHRT_MIN), SHRT_MAX);

    for (int dx = 0; dx < w; dx++) {
        fx = (float)((dx + 0.5) * scale_x - 0.5);
        sx = (int)(floor(fx));
        fx -= sx;

        if (sx < 0) {
            sx = 0;
            fx = 0.f;
        }
        if (sx >= srcw - 1) {
            sx = srcw - 2;
            fx = 1.f;
        }

        xofs[dx] = sx;

        float a0 = (1.f - fx) * INTER_RESIZE_COEF_SCALE;
        float a1 = fx * INTER_RESIZE_COEF_SCALE;

        ialpha[dx * 2]     = SATURATE_CAST_SHORT(a0);
        ialpha[dx * 2 + 1] = SATURATE_CAST_SHORT(a1);
    }

    for (int dy = 0; dy < h; dy++) {
        fy = (float)((dy + 0.5) * scale_y - 0.5);
        sy = (int)(floor(fy));
        fy -= sy;

        if (sy < 0) {
            sy = 0;
            fy = 0.f;
        }
        if (sy >= srch - 1) {
            sy = srch - 2;
            fy = 1.f;
        }

        yofs[dy] = sy;

        float b0 = (1.f - fy) * INTER_RESIZE_COEF_SCALE;
        float b1 = fy * INTER_RESIZE_COEF_SCALE;

        ibeta[dy * 2]     = SATURATE_CAST_SHORT(b0);
        ibeta[dy * 2 + 1] = SATURATE_CAST_SHORT(b1);
    }

#undef SATURATE_CAST_SHORT

    // loop body
    short* rows0 = (short*)base::fast_malloc(w * 2 * sizeof(short));
    short* rows1 = (short*)base::fast_malloc(w * 2 * sizeof(short));

    int prev_sy1 = -2;

    for (int dy = 0; dy < h; dy++) {
        sy = yofs[dy];

        if (sy == prev_sy1) {
            // reuse all rows
        } else if (sy == prev_sy1 + 1) {
            // hresize one row
            short* rows0_old  = rows0;
            rows0             = rows1;
            rows1             = rows0_old;
            const uint8_t* S1 = src + srcstride * (sy + 1);

            const short* ialphap = ialpha;
            short* rows1p        = rows1;
            for (int dx = 0; dx < w; dx++) {
                sx       = xofs[dx];
                short a0 = ialphap[0];
                short a1 = ialphap[1];

                const uint8_t* S1p = S1 + sx;
                rows1p[dx]         = (S1p[0] * a0 + S1p[1] * a1) >> 4;

                ialphap += 2;
            }
        } else {
            // hresize two rows
            const uint8_t* S0 = src + srcstride * (sy);
            const uint8_t* S1 = src + srcstride * (sy + 1);

            const short* ialphap = ialpha;
            short* rows0p        = rows0;
            short* rows1p        = rows1;
            for (int dx = 0; dx < w; dx++) {
                sx       = xofs[dx];
                short a0 = ialphap[0];
                short a1 = ialphap[1];

                const uint8_t* S0p = S0 + sx;
                const uint8_t* S1p = S1 + sx;
                rows0p[dx]         = (S0p[0] * a0 + S0p[1] * a1) >> 4;
                rows1p[dx]         = (S1p[0] * a0 + S1p[1] * a1) >> 4;

                ialphap += 2;
            }
        }

        prev_sy1 = sy;

        // vresize
        short b0 = ibeta[0];
        short b1 = ibeta[1];

        short* rows0p = rows0;
        short* rows1p = rows1;
        uint8_t* Dp   = dst + stride * (dy);

#if __ARM_NEON
        int nn = w >> 3;
#else
        int nn = 0;
#endif
        int remain = w - (nn << 3);

#if __ARM_NEON
#if __aarch64__
        int16x4_t _b0 = vdup_n_s16(b0);
        int16x4_t _b1 = vdup_n_s16(b1);
        int32x4_t _v2 = vdupq_n_s32(2);
        for (; nn > 0; nn--) {
            int16x4_t _rows0p_sr4   = vld1_s16(rows0p);
            int16x4_t _rows1p_sr4   = vld1_s16(rows1p);
            int16x4_t _rows0p_1_sr4 = vld1_s16(rows0p + 4);
            int16x4_t _rows1p_1_sr4 = vld1_s16(rows1p + 4);

            int32x4_t _rows0p_sr4_mb0   = vmull_s16(_rows0p_sr4, _b0);
            int32x4_t _rows1p_sr4_mb1   = vmull_s16(_rows1p_sr4, _b1);
            int32x4_t _rows0p_1_sr4_mb0 = vmull_s16(_rows0p_1_sr4, _b0);
            int32x4_t _rows1p_1_sr4_mb1 = vmull_s16(_rows1p_1_sr4, _b1);

            int32x4_t _acc = _v2;
            _acc           = vsraq_n_s32(_acc, _rows0p_sr4_mb0, 16);
            _acc           = vsraq_n_s32(_acc, _rows1p_sr4_mb1, 16);

            int32x4_t _acc_1 = _v2;
            _acc_1           = vsraq_n_s32(_acc_1, _rows0p_1_sr4_mb0, 16);
            _acc_1           = vsraq_n_s32(_acc_1, _rows1p_1_sr4_mb1, 16);

            int16x4_t _acc16   = vshrn_n_s32(_acc, 2);
            int16x4_t _acc16_1 = vshrn_n_s32(_acc_1, 2);

            uint8x8_t _D = vqmovun_s16(vcombine_s16(_acc16, _acc16_1));

            vst1_u8(Dp, _D);

            Dp += 8;
            rows0p += 8;
            rows1p += 8;
        }
#else
        if (nn > 0) {
            asm volatile(
                "vdup.s16   d16, %8         \n"
                "mov        r4, #2          \n"
                "vdup.s16   d17, %9         \n"
                "vdup.s32   q12, r4         \n"
                "pld        [%0, #128]      \n"
                "vld1.s16   {d2-d3}, [%0 :128]!\n"
                "pld        [%1, #128]      \n"
                "vld1.s16   {d6-d7}, [%1 :128]!\n"
                "0:                         \n"
                "vmull.s16  q0, d2, d16     \n"
                "vmull.s16  q1, d3, d16     \n"
                "vorr.s32   q10, q12, q12   \n"
                "vorr.s32   q11, q12, q12   \n"
                "vmull.s16  q2, d6, d17     \n"
                "vmull.s16  q3, d7, d17     \n"
                "vsra.s32   q10, q0, #16    \n"
                "vsra.s32   q11, q1, #16    \n"
                "pld        [%0, #128]      \n"
                "vld1.s16   {d2-d3}, [%0 :128]!\n"
                "vsra.s32   q10, q2, #16    \n"
                "vsra.s32   q11, q3, #16    \n"
                "pld        [%1, #128]      \n"
                "vld1.s16   {d6-d7}, [%1 :128]!\n"
                "vshrn.s32  d20, q10, #2    \n"
                "vshrn.s32  d21, q11, #2    \n"
                "vqmovun.s16 d20, q10        \n"
                "vst1.8     {d20}, [%2]!    \n"
                "subs       %3, #1          \n"
                "bne        0b              \n"
                "sub        %0, #16         \n"
                "sub        %1, #16         \n"
                : "=r"(rows0p), // %0
                  "=r"(rows1p), // %1
                  "=r"(Dp),     // %2
                  "=r"(nn)      // %3
                : "0"(rows0p),
                  "1"(rows1p),
                  "2"(Dp),
                  "3"(nn),
                  "r"(b0), // %8
                  "r"(b1)  // %9
                : "cc", "memory", "r4", "q0", "q1", "q2", "q3", "q8", "q9", "q10", "q11", "q12");
        }
#endif // __aarch64__
#endif // __ARM_NEON
        for (; remain; --remain) {
            //             D[x] = (rows0[x]*b0 + rows1[x]*b1) >> INTER_RESIZE_COEF_BITS;
            *Dp++ = (uint8_t)(((short)((b0 * (short)(*rows0p++)) >> 16) +
                               (short)((b1 * (short)(*rows1p++)) >> 16) + 2) >>
                              2);
        }

        ibeta += 2;
    }

    base::fast_free(rows0);
    base::fast_free(rows1);

    base::fast_free(buf);
}

void resize_bilinear_c2_impl(const uint8_t* src,
                             uint32_t srcw,
                             uint32_t srch,
                             uint32_t srcstride,
                             uint8_t* dst,
                             uint32_t w,
                             uint32_t h,
                             uint32_t stride) {
    const int INTER_RESIZE_COEF_BITS  = 11;
    const int INTER_RESIZE_COEF_SCALE = 1 << INTER_RESIZE_COEF_BITS;
    //     const int ONE=INTER_RESIZE_COEF_SCALE;

    double scale_x = (double)srcw / w;
    double scale_y = (double)srch / h;

    int* buf = (int*)base::fast_malloc((w + h + w + h) * sizeof(int));

    int* xofs = buf;     // new int[w];
    int* yofs = buf + w; // new int[h];

    short* ialpha = (short*)(buf + w + h);     // new short[w * 2];
    short* ibeta  = (short*)(buf + w + h + w); // new short[h * 2];

    float fx;
    float fy;
    int sx;
    int sy;

#define SATURATE_CAST_SHORT(X) \
    (short)std::min(std::max((int)(X + (X >= 0.f ? 0.5f : -0.5f)), SHRT_MIN), SHRT_MAX);

    for (int dx = 0; dx < w; dx++) {
        fx = (float)((dx + 0.5) * scale_x - 0.5);
        sx = (int)(floor(fx));
        fx -= sx;

        if (sx < 0) {
            sx = 0;
            fx = 0.f;
        }
        if (sx >= srcw - 1) {
            sx = srcw - 2;
            fx = 1.f;
        }

        xofs[dx] = sx * 2;

        float a0 = (1.f - fx) * INTER_RESIZE_COEF_SCALE;
        float a1 = fx * INTER_RESIZE_COEF_SCALE;

        ialpha[dx * 2]     = SATURATE_CAST_SHORT(a0);
        ialpha[dx * 2 + 1] = SATURATE_CAST_SHORT(a1);
    }

    for (int dy = 0; dy < h; dy++) {
        fy = (float)((dy + 0.5) * scale_y - 0.5);
        sy = (int)(floor(fy));
        fy -= sy;

        if (sy < 0) {
            sy = 0;
            fy = 0.f;
        }
        if (sy >= srch - 1) {
            sy = srch - 2;
            fy = 1.f;
        }

        yofs[dy] = sy;

        float b0 = (1.f - fy) * INTER_RESIZE_COEF_SCALE;
        float b1 = fy * INTER_RESIZE_COEF_SCALE;

        ibeta[dy * 2]     = SATURATE_CAST_SHORT(b0);
        ibeta[dy * 2 + 1] = SATURATE_CAST_SHORT(b1);
    }

#undef SATURATE_CAST_SHORT

    // loop body
    short* rows0 = (short*)base::fast_malloc(w * 2 * sizeof(short));
    short* rows1 = (short*)base::fast_malloc(w * 2 * sizeof(short));

    int prev_sy1 = -2;

    for (int dy = 0; dy < h; dy++) {
        sy = yofs[dy];

        if (sy == prev_sy1) {
            // reuse all rows
        } else if (sy == prev_sy1 + 1) {
            // hresize one row
            short* rows0_old  = rows0;
            rows0             = rows1;
            rows1             = rows0_old;
            const uint8_t* S1 = src + srcstride * (sy + 1);

            const short* ialphap = ialpha;
            short* rows1p        = rows1;
            for (int dx = 0; dx < w; dx++) {
                sx = xofs[dx];

                const uint8_t* S1p = S1 + sx;
#if __ARM_NEON
                int16x4_t _a0a1XX   = vld1_s16(ialphap);
                int16x4_t _a0a0a1a1 = vzip_s16(_a0a1XX, _a0a1XX).val[0];
                uint8x8_t _S1       = uint8x8_t();

                _S1 = vld1_lane_u8(S1p, _S1, 0);
                _S1 = vld1_lane_u8(S1p + 1, _S1, 1);
                _S1 = vld1_lane_u8(S1p + 2, _S1, 2);
                _S1 = vld1_lane_u8(S1p + 3, _S1, 3);

                int16x8_t _S116      = vreinterpretq_s16_u16(vmovl_u8(_S1));
                int16x4_t _S1lowhigh = vget_low_s16(_S116);
                int32x4_t _S1ma0a1   = vmull_s16(_S1lowhigh, _a0a0a1a1);
                int32x2_t _rows1low  = vadd_s32(vget_low_s32(_S1ma0a1), vget_high_s32(_S1ma0a1));
                int32x4_t _rows1     = vcombine_s32(_rows1low, vget_high_s32(_S1ma0a1));
                int16x4_t _rows1_sr4 = vshrn_n_s32(_rows1, 4);
                vst1_s16(rows1p, _rows1_sr4);
#else
                short a0 = ialphap[0];
                short a1 = ialphap[1];

                rows1p[0] = (S1p[0] * a0 + S1p[2] * a1) >> 4;
                rows1p[1] = (S1p[1] * a0 + S1p[3] * a1) >> 4;
#endif // __ARM_NEON

                ialphap += 2;
                rows1p += 2;
            }
        } else {
            // hresize two rows
            const uint8_t* S0 = src + srcstride * (sy);
            const uint8_t* S1 = src + srcstride * (sy + 1);

            const short* ialphap = ialpha;
            short* rows0p        = rows0;
            short* rows1p        = rows1;
            for (int dx = 0; dx < w; dx++) {
                sx       = xofs[dx];
                short a0 = ialphap[0];
                short a1 = ialphap[1];

                const uint8_t* S0p = S0 + sx;
                const uint8_t* S1p = S1 + sx;
#if __ARM_NEON
                int16x4_t _a0 = vdup_n_s16(a0);
                int16x4_t _a1 = vdup_n_s16(a1);
                uint8x8_t _S0 = uint8x8_t();
                uint8x8_t _S1 = uint8x8_t();

                _S0 = vld1_lane_u8(S0p, _S0, 0);
                _S0 = vld1_lane_u8(S0p + 1, _S0, 1);
                _S0 = vld1_lane_u8(S0p + 2, _S0, 2);
                _S0 = vld1_lane_u8(S0p + 3, _S0, 3);

                _S1 = vld1_lane_u8(S1p, _S1, 0);
                _S1 = vld1_lane_u8(S1p + 1, _S1, 1);
                _S1 = vld1_lane_u8(S1p + 2, _S1, 2);
                _S1 = vld1_lane_u8(S1p + 3, _S1, 3);

                int16x8_t _S016      = vreinterpretq_s16_u16(vmovl_u8(_S0));
                int16x8_t _S116      = vreinterpretq_s16_u16(vmovl_u8(_S1));
                int16x4_t _S0lowhigh = vget_low_s16(_S016);
                int16x4_t _S1lowhigh = vget_low_s16(_S116);
                int32x2x2_t _S0S1low_S0S1high =
                    vtrn_s32(vreinterpret_s32_s16(_S0lowhigh), vreinterpret_s32_s16(_S1lowhigh));
                int32x4_t _rows01 = vmull_s16(vreinterpret_s16_s32(_S0S1low_S0S1high.val[0]), _a0);
                _rows01 = vmlal_s16(_rows01, vreinterpret_s16_s32(_S0S1low_S0S1high.val[1]), _a1);
                int16x4_t _rows01_sr4 = vshrn_n_s32(_rows01, 4);
                int16x4_t _rows1_sr4  = vext_s16(_rows01_sr4, _rows01_sr4, 2);
                vst1_s16(rows0p, _rows01_sr4);
                vst1_s16(rows1p, _rows1_sr4);
#else
                rows0p[0] = (S0p[0] * a0 + S0p[2] * a1) >> 4;
                rows0p[1] = (S0p[1] * a0 + S0p[3] * a1) >> 4;
                rows1p[0] = (S1p[0] * a0 + S1p[2] * a1) >> 4;
                rows1p[1] = (S1p[1] * a0 + S1p[3] * a1) >> 4;
#endif // __ARM_NEON

                ialphap += 2;
                rows0p += 2;
                rows1p += 2;
            }
        }

        prev_sy1 = sy;

        // vresize
        short b0 = ibeta[0];
        short b1 = ibeta[1];

        short* rows0p = rows0;
        short* rows1p = rows1;
        uint8_t* Dp   = dst + stride * (dy);

#if __ARM_NEON
        int nn = (w * 2) >> 3;
#else
        int nn = 0;
#endif
        int remain = (w * 2) - (nn << 3);

#if __ARM_NEON
#if __aarch64__
        int16x4_t _b0 = vdup_n_s16(b0);
        int16x4_t _b1 = vdup_n_s16(b1);
        int32x4_t _v2 = vdupq_n_s32(2);
        for (; nn > 0; nn--) {
            int16x4_t _rows0p_sr4   = vld1_s16(rows0p);
            int16x4_t _rows1p_sr4   = vld1_s16(rows1p);
            int16x4_t _rows0p_1_sr4 = vld1_s16(rows0p + 4);
            int16x4_t _rows1p_1_sr4 = vld1_s16(rows1p + 4);

            int32x4_t _rows0p_sr4_mb0   = vmull_s16(_rows0p_sr4, _b0);
            int32x4_t _rows1p_sr4_mb1   = vmull_s16(_rows1p_sr4, _b1);
            int32x4_t _rows0p_1_sr4_mb0 = vmull_s16(_rows0p_1_sr4, _b0);
            int32x4_t _rows1p_1_sr4_mb1 = vmull_s16(_rows1p_1_sr4, _b1);

            int32x4_t _acc = _v2;
            _acc           = vsraq_n_s32(_acc, _rows0p_sr4_mb0, 16);
            _acc           = vsraq_n_s32(_acc, _rows1p_sr4_mb1, 16);

            int32x4_t _acc_1 = _v2;
            _acc_1           = vsraq_n_s32(_acc_1, _rows0p_1_sr4_mb0, 16);
            _acc_1           = vsraq_n_s32(_acc_1, _rows1p_1_sr4_mb1, 16);

            int16x4_t _acc16   = vshrn_n_s32(_acc, 2);
            int16x4_t _acc16_1 = vshrn_n_s32(_acc_1, 2);

            uint8x8_t _D = vqmovun_s16(vcombine_s16(_acc16, _acc16_1));

            vst1_u8(Dp, _D);

            Dp += 8;
            rows0p += 8;
            rows1p += 8;
        }
#else
        if (nn > 0) {
            asm volatile(
                "vdup.s16   d16, %8         \n"
                "mov        r4, #2          \n"
                "vdup.s16   d17, %9         \n"
                "vdup.s32   q12, r4         \n"
                "pld        [%0, #128]      \n"
                "vld1.s16   {d2-d3}, [%0 :128]!\n"
                "pld        [%1, #128]      \n"
                "vld1.s16   {d6-d7}, [%1 :128]!\n"
                "0:                         \n"
                "vmull.s16  q0, d2, d16     \n"
                "vmull.s16  q1, d3, d16     \n"
                "vorr.s32   q10, q12, q12   \n"
                "vorr.s32   q11, q12, q12   \n"
                "vmull.s16  q2, d6, d17     \n"
                "vmull.s16  q3, d7, d17     \n"
                "vsra.s32   q10, q0, #16    \n"
                "vsra.s32   q11, q1, #16    \n"
                "pld        [%0, #128]      \n"
                "vld1.s16   {d2-d3}, [%0 :128]!\n"
                "vsra.s32   q10, q2, #16    \n"
                "vsra.s32   q11, q3, #16    \n"
                "pld        [%1, #128]      \n"
                "vld1.s16   {d6-d7}, [%1 :128]!\n"
                "vshrn.s32  d20, q10, #2    \n"
                "vshrn.s32  d21, q11, #2    \n"
                "vqmovun.s16 d20, q10        \n"
                "vst1.8     {d20}, [%2]!    \n"
                "subs       %3, #1          \n"
                "bne        0b              \n"
                "sub        %0, #16         \n"
                "sub        %1, #16         \n"
                : "=r"(rows0p), // %0
                  "=r"(rows1p), // %1
                  "=r"(Dp),     // %2
                  "=r"(nn)      // %3
                : "0"(rows0p),
                  "1"(rows1p),
                  "2"(Dp),
                  "3"(nn),
                  "r"(b0), // %8
                  "r"(b1)  // %9
                : "cc", "memory", "r4", "q0", "q1", "q2", "q3", "q8", "q9", "q10", "q11", "q12");
        }
#endif // __aarch64__
#endif // __ARM_NEON
        for (; remain; --remain) {
            //             D[x] = (rows0[x]*b0 + rows1[x]*b1) >> INTER_RESIZE_COEF_BITS;
            *Dp++ = (uint8_t)(((short)((b0 * (short)(*rows0p++)) >> 16) +
                               (short)((b1 * (short)(*rows1p++)) >> 16) + 2) >>
                              2);
        }

        ibeta += 2;
    }

    base::fast_free(rows0);
    base::fast_free(rows1);

    base::fast_free(buf);
}

void resize_bilinear_c3_impl(const uint8_t* src,
                             uint32_t srcw,
                             uint32_t srch,
                             uint32_t srcstride,
                             uint8_t* dst,
                             uint32_t w,
                             uint32_t h,
                             uint32_t stride) {
    const int INTER_RESIZE_COEF_BITS  = 11;
    const int INTER_RESIZE_COEF_SCALE = 1 << INTER_RESIZE_COEF_BITS;
    //     const int ONE=INTER_RESIZE_COEF_SCALE;

    double scale_x = (double)srcw / w;
    double scale_y = (double)srch / h;

    int* buf = (int*)base::fast_malloc((w + h + w + h) * sizeof(int));

    int* xofs = buf;     // new int[w];
    int* yofs = buf + w; // new int[h];

    short* ialpha = (short*)(buf + w + h);     // new short[w * 2];
    short* ibeta  = (short*)(buf + w + h + w); // new short[h * 2];

    float fx;
    float fy;
    int sx;
    int sy;

#define SATURATE_CAST_SHORT(X) \
    (short)std::min(std::max((int)(X + (X >= 0.f ? 0.5f : -0.5f)), SHRT_MIN), SHRT_MAX);

    for (int dx = 0; dx < w; dx++) {
        fx = (float)((dx + 0.5) * scale_x - 0.5);
        sx = (int)(floor(fx));
        fx -= sx;

        if (sx < 0) {
            sx = 0;
            fx = 0.f;
        }
        if (sx >= srcw - 1) {
            sx = srcw - 2;
            fx = 1.f;
        }

        xofs[dx] = sx * 3;

        float a0 = (1.f - fx) * INTER_RESIZE_COEF_SCALE;
        float a1 = fx * INTER_RESIZE_COEF_SCALE;

        ialpha[dx * 2]     = SATURATE_CAST_SHORT(a0);
        ialpha[dx * 2 + 1] = SATURATE_CAST_SHORT(a1);
    }

    for (int dy = 0; dy < h; dy++) {
        fy = (float)((dy + 0.5) * scale_y - 0.5);
        sy = (int)(floor(fy));
        fy -= sy;

        if (sy < 0) {
            sy = 0;
            fy = 0.f;
        }
        if (sy >= srch - 1) {
            sy = srch - 2;
            fy = 1.f;
        }

        yofs[dy] = sy;

        float b0 = (1.f - fy) * INTER_RESIZE_COEF_SCALE;
        float b1 = fy * INTER_RESIZE_COEF_SCALE;

        ibeta[dy * 2]     = SATURATE_CAST_SHORT(b0);
        ibeta[dy * 2 + 1] = SATURATE_CAST_SHORT(b1);
    }

#undef SATURATE_CAST_SHORT

    // loop body
    short* rows0 = (short*)base::fast_malloc((w * 3 + 1) * 2 * sizeof(short));
    short* rows1 = (short*)base::fast_malloc((w * 3 + 1) * 2 * sizeof(short));

    int prev_sy1 = -2;

    for (int dy = 0; dy < h; dy++) {
        sy = yofs[dy];

        if (sy == prev_sy1) {
            // reuse all rows
        } else if (sy == prev_sy1 + 1) {
            // hresize one row
            short* rows0_old  = rows0;
            rows0             = rows1;
            rows1             = rows0_old;
            const uint8_t* S1 = src + srcstride * (sy + 1);

            const short* ialphap = ialpha;
            short* rows1p        = rows1;
            for (int dx = 0; dx < w; dx++) {
                sx       = xofs[dx];
                short a0 = ialphap[0];
                short a1 = ialphap[1];

                const uint8_t* S1p = S1 + sx;
#if __ARM_NEON
                int16x4_t _a0 = vdup_n_s16(a0);
                int16x4_t _a1 = vdup_n_s16(a1);
                uint8x8_t _S1 = uint8x8_t();

                _S1 = vld1_lane_u8(S1p, _S1, 0);
                _S1 = vld1_lane_u8(S1p + 1, _S1, 1);
                _S1 = vld1_lane_u8(S1p + 2, _S1, 2);
                _S1 = vld1_lane_u8(S1p + 3, _S1, 3);
                _S1 = vld1_lane_u8(S1p + 4, _S1, 4);
                _S1 = vld1_lane_u8(S1p + 5, _S1, 5);

                int16x8_t _S116      = vreinterpretq_s16_u16(vmovl_u8(_S1));
                int16x4_t _S1low     = vget_low_s16(_S116);
                int16x4_t _S1high    = vext_s16(_S1low, vget_high_s16(_S116), 3);
                int32x4_t _rows1     = vmull_s16(_S1low, _a0);
                _rows1               = vmlal_s16(_rows1, _S1high, _a1);
                int16x4_t _rows1_sr4 = vshrn_n_s32(_rows1, 4);
                vst1_s16(rows1p, _rows1_sr4);
#else
                rows1p[0] = (S1p[0] * a0 + S1p[3] * a1) >> 4;
                rows1p[1] = (S1p[1] * a0 + S1p[4] * a1) >> 4;
                rows1p[2] = (S1p[2] * a0 + S1p[5] * a1) >> 4;
#endif // __ARM_NEON

                ialphap += 2;
                rows1p += 3;
            }
        } else {
            // hresize two rows
            const uint8_t* S0 = src + srcstride * (sy);
            const uint8_t* S1 = src + srcstride * (sy + 1);

            const short* ialphap = ialpha;
            short* rows0p        = rows0;
            short* rows1p        = rows1;
            for (int dx = 0; dx < w; dx++) {
                sx       = xofs[dx];
                short a0 = ialphap[0];
                short a1 = ialphap[1];

                const uint8_t* S0p = S0 + sx;
                const uint8_t* S1p = S1 + sx;
#if __ARM_NEON
                int16x4_t _a0 = vdup_n_s16(a0);
                int16x4_t _a1 = vdup_n_s16(a1);
                uint8x8_t _S0 = uint8x8_t();
                uint8x8_t _S1 = uint8x8_t();

                _S0 = vld1_lane_u8(S0p, _S0, 0);
                _S0 = vld1_lane_u8(S0p + 1, _S0, 1);
                _S0 = vld1_lane_u8(S0p + 2, _S0, 2);
                _S0 = vld1_lane_u8(S0p + 3, _S0, 3);
                _S0 = vld1_lane_u8(S0p + 4, _S0, 4);
                _S0 = vld1_lane_u8(S0p + 5, _S0, 5);

                _S1 = vld1_lane_u8(S1p, _S1, 0);
                _S1 = vld1_lane_u8(S1p + 1, _S1, 1);
                _S1 = vld1_lane_u8(S1p + 2, _S1, 2);
                _S1 = vld1_lane_u8(S1p + 3, _S1, 3);
                _S1 = vld1_lane_u8(S1p + 4, _S1, 4);
                _S1 = vld1_lane_u8(S1p + 5, _S1, 5);

                int16x8_t _S016      = vreinterpretq_s16_u16(vmovl_u8(_S0));
                int16x8_t _S116      = vreinterpretq_s16_u16(vmovl_u8(_S1));
                int16x4_t _S0low     = vget_low_s16(_S016);
                int16x4_t _S1low     = vget_low_s16(_S116);
                int16x4_t _S0high    = vext_s16(_S0low, vget_high_s16(_S016), 3);
                int16x4_t _S1high    = vext_s16(_S1low, vget_high_s16(_S116), 3);
                int32x4_t _rows0     = vmull_s16(_S0low, _a0);
                int32x4_t _rows1     = vmull_s16(_S1low, _a0);
                _rows0               = vmlal_s16(_rows0, _S0high, _a1);
                _rows1               = vmlal_s16(_rows1, _S1high, _a1);
                int16x4_t _rows0_sr4 = vshrn_n_s32(_rows0, 4);
                int16x4_t _rows1_sr4 = vshrn_n_s32(_rows1, 4);
                vst1_s16(rows0p, _rows0_sr4);
                vst1_s16(rows1p, _rows1_sr4);
#else
                rows0p[0] = (S0p[0] * a0 + S0p[3] * a1) >> 4;
                rows0p[1] = (S0p[1] * a0 + S0p[4] * a1) >> 4;
                rows0p[2] = (S0p[2] * a0 + S0p[5] * a1) >> 4;
                rows1p[0] = (S1p[0] * a0 + S1p[3] * a1) >> 4;
                rows1p[1] = (S1p[1] * a0 + S1p[4] * a1) >> 4;
                rows1p[2] = (S1p[2] * a0 + S1p[5] * a1) >> 4;
#endif // __ARM_NEON

                ialphap += 2;
                rows0p += 3;
                rows1p += 3;
            }
        }

        prev_sy1 = sy;

        // vresize
        short b0 = ibeta[0];
        short b1 = ibeta[1];

        short* rows0p = rows0;
        short* rows1p = rows1;
        uint8_t* Dp   = dst + stride * (dy);

#if __ARM_NEON
        int nn = (w * 3) >> 3;
#else
        int nn = 0;
#endif
        int remain = (w * 3) - (nn << 3);

#if __ARM_NEON
#if __aarch64__
        int16x4_t _b0 = vdup_n_s16(b0);
        int16x4_t _b1 = vdup_n_s16(b1);
        int32x4_t _v2 = vdupq_n_s32(2);
        for (; nn > 0; nn--) {
            int16x4_t _rows0p_sr4   = vld1_s16(rows0p);
            int16x4_t _rows1p_sr4   = vld1_s16(rows1p);
            int16x4_t _rows0p_1_sr4 = vld1_s16(rows0p + 4);
            int16x4_t _rows1p_1_sr4 = vld1_s16(rows1p + 4);

            int32x4_t _rows0p_sr4_mb0   = vmull_s16(_rows0p_sr4, _b0);
            int32x4_t _rows1p_sr4_mb1   = vmull_s16(_rows1p_sr4, _b1);
            int32x4_t _rows0p_1_sr4_mb0 = vmull_s16(_rows0p_1_sr4, _b0);
            int32x4_t _rows1p_1_sr4_mb1 = vmull_s16(_rows1p_1_sr4, _b1);

            int32x4_t _acc = _v2;
            _acc           = vsraq_n_s32(_acc, _rows0p_sr4_mb0, 16);
            _acc           = vsraq_n_s32(_acc, _rows1p_sr4_mb1, 16);

            int32x4_t _acc_1 = _v2;
            _acc_1           = vsraq_n_s32(_acc_1, _rows0p_1_sr4_mb0, 16);
            _acc_1           = vsraq_n_s32(_acc_1, _rows1p_1_sr4_mb1, 16);

            int16x4_t _acc16   = vshrn_n_s32(_acc, 2);
            int16x4_t _acc16_1 = vshrn_n_s32(_acc_1, 2);

            uint8x8_t _D = vqmovun_s16(vcombine_s16(_acc16, _acc16_1));

            vst1_u8(Dp, _D);

            Dp += 8;
            rows0p += 8;
            rows1p += 8;
        }
#else
        if (nn > 0) {
            asm volatile(
                "vdup.s16   d16, %8         \n"
                "mov        r4, #2          \n"
                "vdup.s16   d17, %9         \n"
                "vdup.s32   q12, r4         \n"
                "pld        [%0, #128]      \n"
                "vld1.s16   {d2-d3}, [%0 :128]!\n"
                "pld        [%1, #128]      \n"
                "vld1.s16   {d6-d7}, [%1 :128]!\n"
                "0:                         \n"
                "vmull.s16  q0, d2, d16     \n"
                "vmull.s16  q1, d3, d16     \n"
                "vorr.s32   q10, q12, q12   \n"
                "vorr.s32   q11, q12, q12   \n"
                "vmull.s16  q2, d6, d17     \n"
                "vmull.s16  q3, d7, d17     \n"
                "vsra.s32   q10, q0, #16    \n"
                "vsra.s32   q11, q1, #16    \n"
                "pld        [%0, #128]      \n"
                "vld1.s16   {d2-d3}, [%0 :128]!\n"
                "vsra.s32   q10, q2, #16    \n"
                "vsra.s32   q11, q3, #16    \n"
                "pld        [%1, #128]      \n"
                "vld1.s16   {d6-d7}, [%1 :128]!\n"
                "vshrn.s32  d20, q10, #2    \n"
                "vshrn.s32  d21, q11, #2    \n"
                "vqmovun.s16 d20, q10        \n"
                "vst1.8     {d20}, [%2]!    \n"
                "subs       %3, #1          \n"
                "bne        0b              \n"
                "sub        %0, #16         \n"
                "sub        %1, #16         \n"
                : "=r"(rows0p), // %0
                  "=r"(rows1p), // %1
                  "=r"(Dp),     // %2
                  "=r"(nn)      // %3
                : "0"(rows0p),
                  "1"(rows1p),
                  "2"(Dp),
                  "3"(nn),
                  "r"(b0), // %8
                  "r"(b1)  // %9
                : "cc", "memory", "r4", "q0", "q1", "q2", "q3", "q8", "q9", "q10", "q11", "q12");
        }
#endif // __aarch64__
#endif // __ARM_NEON
        for (; remain; --remain) {
            //             D[x] = (rows0[x]*b0 + rows1[x]*b1) >> INTER_RESIZE_COEF_BITS;
            *Dp++ = (uint8_t)(((short)((b0 * (short)(*rows0p++)) >> 16) +
                               (short)((b1 * (short)(*rows1p++)) >> 16) + 2) >>
                              2);
        }

        ibeta += 2;
    }

    base::fast_free(rows0);
    base::fast_free(rows1);

    base::fast_free(buf);
}

void resize_bilinear_c4_impl(const uint8_t* src,
                             int srcw,
                             int srch,
                             int srcstride,
                             uint8_t* dst,
                             int w,
                             int h,
                             int stride) {
    const int INTER_RESIZE_COEF_BITS  = 11;
    const int INTER_RESIZE_COEF_SCALE = 1 << INTER_RESIZE_COEF_BITS;
    //     const int ONE=INTER_RESIZE_COEF_SCALE;

    double scale_x = (double)srcw / w;
    double scale_y = (double)srch / h;

    int* buf = (int*)base::fast_malloc((w + h + w + h) * sizeof(int));

    int* xofs = buf;     // new int[w];
    int* yofs = buf + w; // new int[h];

    short* ialpha = (short*)(buf + w + h);     // new short[w * 2];
    short* ibeta  = (short*)(buf + w + h + w); // new short[h * 2];

    float fx;
    float fy;
    int sx;
    int sy;

#define SATURATE_CAST_SHORT(X) \
    (short)std::min(std::max((int)(X + (X >= 0.f ? 0.5f : -0.5f)), SHRT_MIN), SHRT_MAX);

    for (int dx = 0; dx < w; dx++) {
        fx = (float)((dx + 0.5) * scale_x - 0.5);
        sx = (int)(floor(fx));
        fx -= sx;

        if (sx < 0) {
            sx = 0;
            fx = 0.f;
        }
        if (sx >= srcw - 1) {
            sx = srcw - 2;
            fx = 1.f;
        }

        xofs[dx] = sx * 4;

        float a0 = (1.f - fx) * INTER_RESIZE_COEF_SCALE;
        float a1 = fx * INTER_RESIZE_COEF_SCALE;

        ialpha[dx * 2]     = SATURATE_CAST_SHORT(a0);
        ialpha[dx * 2 + 1] = SATURATE_CAST_SHORT(a1);
    }

    for (int dy = 0; dy < h; dy++) {
        fy = (float)((dy + 0.5) * scale_y - 0.5);
        sy = (int)(floor(fy));
        fy -= sy;

        if (sy < 0) {
            sy = 0;
            fy = 0.f;
        }
        if (sy >= srch - 1) {
            sy = srch - 2;
            fy = 1.f;
        }

        yofs[dy] = sy;

        float b0 = (1.f - fy) * INTER_RESIZE_COEF_SCALE;
        float b1 = fy * INTER_RESIZE_COEF_SCALE;

        ibeta[dy * 2]     = SATURATE_CAST_SHORT(b0);
        ibeta[dy * 2 + 1] = SATURATE_CAST_SHORT(b1);
    }

#undef SATURATE_CAST_SHORT

    // loop body
    short* rows0 = (short*)base::fast_malloc(w * 4 * 2 * sizeof(short));
    short* rows1 = (short*)base::fast_malloc(w * 4 * 2 * sizeof(short));

    int prev_sy1 = -2;

    for (int dy = 0; dy < h; dy++) {
        sy = yofs[dy];

        if (sy == prev_sy1) {
            // reuse all rows
        } else if (sy == prev_sy1 + 1) {
            // hresize one row
            short* rows0_old  = rows0;
            rows0             = rows1;
            rows1             = rows0_old;
            const uint8_t* S1 = src + srcstride * (sy + 1);

            const short* ialphap = ialpha;
            short* rows1p        = rows1;
            for (int dx = 0; dx < w; dx++) {
                sx       = xofs[dx];
                short a0 = ialphap[0];
                short a1 = ialphap[1];

                const uint8_t* S1p = S1 + sx;
#if __ARM_NEON
                int16x4_t _a0        = vdup_n_s16(a0);
                int16x4_t _a1        = vdup_n_s16(a1);
                uint8x8_t _S1        = vld1_u8(S1p);
                int16x8_t _S116      = vreinterpretq_s16_u16(vmovl_u8(_S1));
                int16x4_t _S1low     = vget_low_s16(_S116);
                int16x4_t _S1high    = vget_high_s16(_S116);
                int32x4_t _rows1     = vmull_s16(_S1low, _a0);
                _rows1               = vmlal_s16(_rows1, _S1high, _a1);
                int16x4_t _rows1_sr4 = vshrn_n_s32(_rows1, 4);
                vst1_s16(rows1p, _rows1_sr4);
#else
                rows1p[0] = (S1p[0] * a0 + S1p[4] * a1) >> 4;
                rows1p[1] = (S1p[1] * a0 + S1p[5] * a1) >> 4;
                rows1p[2] = (S1p[2] * a0 + S1p[6] * a1) >> 4;
                rows1p[3] = (S1p[3] * a0 + S1p[7] * a1) >> 4;
#endif // __ARM_NEON

                ialphap += 2;
                rows1p += 4;
            }
        } else {
            // hresize two rows
            const uint8_t* S0 = src + srcstride * (sy);
            const uint8_t* S1 = src + srcstride * (sy + 1);

            const short* ialphap = ialpha;
            short* rows0p        = rows0;
            short* rows1p        = rows1;
            for (int dx = 0; dx < w; dx++) {
                sx       = xofs[dx];
                short a0 = ialphap[0];
                short a1 = ialphap[1];

                const uint8_t* S0p = S0 + sx;
                const uint8_t* S1p = S1 + sx;
#if __ARM_NEON
                int16x4_t _a0        = vdup_n_s16(a0);
                int16x4_t _a1        = vdup_n_s16(a1);
                uint8x8_t _S0        = vld1_u8(S0p);
                uint8x8_t _S1        = vld1_u8(S1p);
                int16x8_t _S016      = vreinterpretq_s16_u16(vmovl_u8(_S0));
                int16x8_t _S116      = vreinterpretq_s16_u16(vmovl_u8(_S1));
                int16x4_t _S0low     = vget_low_s16(_S016);
                int16x4_t _S1low     = vget_low_s16(_S116);
                int16x4_t _S0high    = vget_high_s16(_S016);
                int16x4_t _S1high    = vget_high_s16(_S116);
                int32x4_t _rows0     = vmull_s16(_S0low, _a0);
                int32x4_t _rows1     = vmull_s16(_S1low, _a0);
                _rows0               = vmlal_s16(_rows0, _S0high, _a1);
                _rows1               = vmlal_s16(_rows1, _S1high, _a1);
                int16x4_t _rows0_sr4 = vshrn_n_s32(_rows0, 4);
                int16x4_t _rows1_sr4 = vshrn_n_s32(_rows1, 4);
                vst1_s16(rows0p, _rows0_sr4);
                vst1_s16(rows1p, _rows1_sr4);
#else
                rows0p[0] = (S0p[0] * a0 + S0p[4] * a1) >> 4;
                rows0p[1] = (S0p[1] * a0 + S0p[5] * a1) >> 4;
                rows0p[2] = (S0p[2] * a0 + S0p[6] * a1) >> 4;
                rows0p[3] = (S0p[3] * a0 + S0p[7] * a1) >> 4;
                rows1p[0] = (S1p[0] * a0 + S1p[4] * a1) >> 4;
                rows1p[1] = (S1p[1] * a0 + S1p[5] * a1) >> 4;
                rows1p[2] = (S1p[2] * a0 + S1p[6] * a1) >> 4;
                rows1p[3] = (S1p[3] * a0 + S1p[7] * a1) >> 4;
#endif // __ARM_NEON

                ialphap += 2;
                rows0p += 4;
                rows1p += 4;
            }
        }

        prev_sy1 = sy;

        // vresize
        short b0 = ibeta[0];
        short b1 = ibeta[1];

        short* rows0p = rows0;
        short* rows1p = rows1;
        uint8_t* Dp   = dst + stride * (dy);

#if __ARM_NEON
        int nn = (w * 4) >> 3;
#else
        int nn = 0;
#endif
        int remain = (w * 4) - (nn << 3);

#if __ARM_NEON
#if __aarch64__
        int16x4_t _b0 = vdup_n_s16(b0);
        int16x4_t _b1 = vdup_n_s16(b1);
        int32x4_t _v2 = vdupq_n_s32(2);
        for (; nn > 0; nn--) {
            int16x4_t _rows0p_sr4   = vld1_s16(rows0p);
            int16x4_t _rows1p_sr4   = vld1_s16(rows1p);
            int16x4_t _rows0p_1_sr4 = vld1_s16(rows0p + 4);
            int16x4_t _rows1p_1_sr4 = vld1_s16(rows1p + 4);

            int32x4_t _rows0p_sr4_mb0   = vmull_s16(_rows0p_sr4, _b0);
            int32x4_t _rows1p_sr4_mb1   = vmull_s16(_rows1p_sr4, _b1);
            int32x4_t _rows0p_1_sr4_mb0 = vmull_s16(_rows0p_1_sr4, _b0);
            int32x4_t _rows1p_1_sr4_mb1 = vmull_s16(_rows1p_1_sr4, _b1);

            int32x4_t _acc = _v2;
            _acc           = vsraq_n_s32(_acc, _rows0p_sr4_mb0, 16);
            _acc           = vsraq_n_s32(_acc, _rows1p_sr4_mb1, 16);

            int32x4_t _acc_1 = _v2;
            _acc_1           = vsraq_n_s32(_acc_1, _rows0p_1_sr4_mb0, 16);
            _acc_1           = vsraq_n_s32(_acc_1, _rows1p_1_sr4_mb1, 16);

            int16x4_t _acc16   = vshrn_n_s32(_acc, 2);
            int16x4_t _acc16_1 = vshrn_n_s32(_acc_1, 2);

            uint8x8_t _D = vqmovun_s16(vcombine_s16(_acc16, _acc16_1));

            vst1_u8(Dp, _D);

            Dp += 8;
            rows0p += 8;
            rows1p += 8;
        }
#else
        if (nn > 0) {
            asm volatile(
                "vdup.s16   d16, %8         \n"
                "mov        r4, #2          \n"
                "vdup.s16   d17, %9         \n"
                "vdup.s32   q12, r4         \n"
                "pld        [%0, #128]      \n"
                "vld1.s16   {d2-d3}, [%0 :128]!\n"
                "pld        [%1, #128]      \n"
                "vld1.s16   {d6-d7}, [%1 :128]!\n"
                "0:                         \n"
                "vmull.s16  q0, d2, d16     \n"
                "vmull.s16  q1, d3, d16     \n"
                "vorr.s32   q10, q12, q12   \n"
                "vorr.s32   q11, q12, q12   \n"
                "vmull.s16  q2, d6, d17     \n"
                "vmull.s16  q3, d7, d17     \n"
                "vsra.s32   q10, q0, #16    \n"
                "vsra.s32   q11, q1, #16    \n"
                "pld        [%0, #128]      \n"
                "vld1.s16   {d2-d3}, [%0 :128]!\n"
                "vsra.s32   q10, q2, #16    \n"
                "vsra.s32   q11, q3, #16    \n"
                "pld        [%1, #128]      \n"
                "vld1.s16   {d6-d7}, [%1 :128]!\n"
                "vshrn.s32  d20, q10, #2    \n"
                "vshrn.s32  d21, q11, #2    \n"
                "vqmovun.s16 d20, q10        \n"
                "vst1.8     {d20}, [%2]!    \n"
                "subs       %3, #1          \n"
                "bne        0b              \n"
                "sub        %0, #16         \n"
                "sub        %1, #16         \n"
                : "=r"(rows0p), // %0
                  "=r"(rows1p), // %1
                  "=r"(Dp),     // %2
                  "=r"(nn)      // %3
                : "0"(rows0p),
                  "1"(rows1p),
                  "2"(Dp),
                  "3"(nn),
                  "r"(b0), // %8
                  "r"(b1)  // %9
                : "cc", "memory", "r4", "q0", "q1", "q2", "q3", "q8", "q9", "q10", "q11", "q12");
        }
#endif // __aarch64__
#endif // __ARM_NEON
        for (; remain; --remain) {
            //             D[x] = (rows0[x]*b0 + rows1[x]*b1) >> INTER_RESIZE_COEF_BITS;
            *Dp++ = (uint8_t)(((short)((b0 * (short)(*rows0p++)) >> 16) +
                               (short)((b1 * (short)(*rows1p++)) >> 16) + 2) >>
                              2);
        }

        ibeta += 2;
    }

    base::fast_free(rows0);
    base::fast_free(rows1);

    base::fast_free(buf);
}

void resize_bilinear_c1(const uint8_t* src,
                        uint32_t srcw,
                        uint32_t srch,
                        uint8_t* dst,
                        uint32_t w,
                        uint32_t h) {
    return resize_bilinear_c1_impl(src, srcw, srch, srcw, dst, w, h, w);
}

void resize_bilinear_c2(const uint8_t* src,
                        uint32_t srcw,
                        uint32_t srch,
                        uint8_t* dst,
                        uint32_t w,
                        uint32_t h) {
    return resize_bilinear_c2_impl(src, srcw, srch, srcw * 2, dst, w, h, w * 2);
}

void resize_bilinear_c3(const uint8_t* src,
                        uint32_t srcw,
                        uint32_t srch,
                        uint8_t* dst,
                        uint32_t w,
                        uint32_t h) {
    return resize_bilinear_c3_impl(src, srcw, srch, srcw * 3, dst, w, h, w * 3);
}

void resize_bilinear_c4(const uint8_t* src,
                        uint32_t srcw,
                        uint32_t srch,
                        uint8_t* dst,
                        uint32_t w,
                        uint32_t h) {
    return resize_bilinear_c3_impl(src, srcw, srch, srcw * 4, dst, w, h, w * 4);
}

void resize_bilinear_yuv420sp(const uint8_t* src,
                              uint32_t srcw,
                              uint32_t srch,
                              uint8_t* dst,
                              uint32_t w,
                              uint32_t h) {
    // assert srcw % 2 == 0
    // assert srch % 2 == 0
    // assert w % 2 == 0
    // assert h % 2 == 0

    const uint8_t* srcY = src;
    uint8_t* dstY       = dst;
    resize_bilinear_c1(srcY, srcw, srch, dstY, w, h);

    const uint8_t* srcUV = src + srcw * srch;
    uint8_t* dstUV       = dst + w * h;
    resize_bilinear_c2(srcUV, srcw / 2, srch / 2, dstUV, w / 2, h / 2);
}
} // namespace cv