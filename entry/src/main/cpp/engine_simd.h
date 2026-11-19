// x86 (PC) の AVX2 + FMA 版の計算。engine.cpp が、CPU が AVX2 と FMA を持っているときだけ使う。
// GCC / Clang では engine.cpp がこのまま読む (関数ごとに target("avx2,fma") を付けるので、ほかの部分は古い命令のまま)。
// MSVC では engine_avx2.cpp (/arch:AVX2 でビルド) だけが読む。そのとき、ここで標準ライブラリの関数やテンプレートを使うと、
// AVX2 で作られた同じ名前の関数がほかのファイルの呼び出しにも使われて古い CPU で落ちることがあるので、
// ここでは組み込み命令 (intrinsics) だけを使い、関数はすべて static にする。
// どれも engine.cpp の普通の版と同じ計算 (足す順と FMA の丸めの違いで最後の桁が違うことはある)。
#pragma once
#include <immintrin.h>
#include <stddef.h>

#ifndef KKC_AVX2_FN
#define KKC_AVX2_FN
#endif

// e^x を 8 本ずつ (Cephes の expf と同じ式。誤差は float の丸めと同じくらい)
static KKC_AVX2_FN inline __m256 kkc_exp8(__m256 x) {
    x = _mm256_min_ps(_mm256_max_ps(x, _mm256_set1_ps(-87.3f)), _mm256_set1_ps(88.3f));
    const __m256 fx = _mm256_floor_ps(_mm256_fmadd_ps(x, _mm256_set1_ps(1.44269504088896341f), _mm256_set1_ps(0.5f)));
    x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(0.693359375f), x);
    x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(-2.12194440e-4f), x);
    __m256 y = _mm256_set1_ps(1.9875691500e-4f);
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.3981999507e-3f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(8.3334519073e-3f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(4.1665795894e-2f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.6666665459e-1f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(5.0000001201e-1f));
    y = _mm256_fmadd_ps(y, _mm256_mul_ps(x, x), _mm256_add_ps(x, _mm256_set1_ps(1.0f)));
    const __m256i e = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvtps_epi32(fx), _mm256_set1_epi32(127)), 23);
    return _mm256_mul_ps(y, _mm256_castsi256_ps(e));
}

static KKC_AVX2_FN inline float kkc_hsum8(__m256 a) {
    __m128 s = _mm_add_ps(_mm256_castps256_ps128(a), _mm256_extractf128_ps(a, 1));
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
}

// lin_part と同じ: Y[t][o] = Σ_i X[t][i] · WT[i][o] + B[o]。出力は [o0, o0 + 16·k) の 16 本単位の所だけ (残りは呼ぶ側)。
// 行は 6 本ずつ (アキュムレータ 12 本 + 重み 2 本 + 入力 1 本 = 16 本のレジスタに収まる)。行が 1 本のとき (区間・語ごとの小さい掛け算) は
// 入力を 2 つずつ進めて、アキュムレータを 2 組にする (足し算の待ちを減らす)
static KKC_AVX2_FN void kkc_simd_lin(const float* X, int T, int in, const float* WT, int ldw, const float* B, float* Y, int ldy,
                                     int o0, int o1) {
    for (int o = o0; o + 16 <= o1; o += 16) {
        const __m256 b0 = B ? _mm256_loadu_ps(B + o) : _mm256_setzero_ps();
        const __m256 b1 = B ? _mm256_loadu_ps(B + o + 8) : _mm256_setzero_ps();
        if (T == 1) {
            __m256 a0 = b0, a1 = b1, c0 = _mm256_setzero_ps(), c1 = _mm256_setzero_ps();
            int i = 0;
            for (; i + 2 <= in; i += 2) {
                const float* w = WT + (size_t)i * ldw + o;
                const __m256 v = _mm256_broadcast_ss(X + i), u = _mm256_broadcast_ss(X + i + 1);
                a0 = _mm256_fmadd_ps(v, _mm256_loadu_ps(w), a0);
                a1 = _mm256_fmadd_ps(v, _mm256_loadu_ps(w + 8), a1);
                c0 = _mm256_fmadd_ps(u, _mm256_loadu_ps(w + ldw), c0);
                c1 = _mm256_fmadd_ps(u, _mm256_loadu_ps(w + ldw + 8), c1);
            }
            for (; i < in; i++) {
                const float* w = WT + (size_t)i * ldw + o;
                const __m256 v = _mm256_broadcast_ss(X + i);
                a0 = _mm256_fmadd_ps(v, _mm256_loadu_ps(w), a0);
                a1 = _mm256_fmadd_ps(v, _mm256_loadu_ps(w + 8), a1);
            }
            _mm256_storeu_ps(Y + o, _mm256_add_ps(a0, c0));
            _mm256_storeu_ps(Y + o + 8, _mm256_add_ps(a1, c1));
            continue;
        }
        for (int t = 0; t < T; t += 6) {
            const int tn = T - t < 6 ? T - t : 6;
            const float* x[6];
            for (int r = 0; r < 6; r++) x[r] = X + (size_t)(t + (r < tn ? r : tn - 1)) * in;
            __m256 a00 = b0, a01 = b1, a10 = b0, a11 = b1, a20 = b0, a21 = b1;
            __m256 a30 = b0, a31 = b1, a40 = b0, a41 = b1, a50 = b0, a51 = b1;
            for (int i = 0; i < in; i++) {
                const float* w = WT + (size_t)i * ldw + o;
                const __m256 w0 = _mm256_loadu_ps(w), w1 = _mm256_loadu_ps(w + 8);
                __m256 v = _mm256_broadcast_ss(x[0] + i); a00 = _mm256_fmadd_ps(v, w0, a00); a01 = _mm256_fmadd_ps(v, w1, a01);
                v = _mm256_broadcast_ss(x[1] + i); a10 = _mm256_fmadd_ps(v, w0, a10); a11 = _mm256_fmadd_ps(v, w1, a11);
                v = _mm256_broadcast_ss(x[2] + i); a20 = _mm256_fmadd_ps(v, w0, a20); a21 = _mm256_fmadd_ps(v, w1, a21);
                v = _mm256_broadcast_ss(x[3] + i); a30 = _mm256_fmadd_ps(v, w0, a30); a31 = _mm256_fmadd_ps(v, w1, a31);
                v = _mm256_broadcast_ss(x[4] + i); a40 = _mm256_fmadd_ps(v, w0, a40); a41 = _mm256_fmadd_ps(v, w1, a41);
                v = _mm256_broadcast_ss(x[5] + i); a50 = _mm256_fmadd_ps(v, w0, a50); a51 = _mm256_fmadd_ps(v, w1, a51);
            }
            float* y = Y + (size_t)t * ldy + o;
            const __m256 acc[12] = {a00, a01, a10, a11, a20, a21, a30, a31, a40, a41, a50, a51};
            for (int r = 0; r < tn; r++) {
                _mm256_storeu_ps(y + (size_t)r * ldy, acc[2 * r]);
                _mm256_storeu_ps(y + (size_t)r * ldy + 8, acc[2 * r + 1]);
            }
        }
    }
}

// ---------------------------------------------------------------- 整数版の全結合 (重みは ±KKC_QW、入力は ±KKC_QX の段階に丸める)
// 16 ビット整数の「2 つずつ掛けて足す」命令 (vpmaddwd) は、8 ビットに丸めても 11 ビットに丸めても速さが同じなので、
// 重みは ±511、入力は ±2047 に丸める (dev・AJIMEE・日常の 9305 文で、float 版と 1 位が違ったのは 2 文。±127 同士の 8 ビットでは 13 文)。
// 合計は 511 × 2047 × 入力の数 (2048 で 21.4 億) で 32 ビットに収まる (入力の数が 2048 を超えるモデルには、呼ぶ側で使わない)
#ifndef KKC_QX
#define KKC_QX 2047   // 入力の段階 (±)
#endif
#ifndef KKC_QW
#define KKC_QW 511    // 重みの段階 (±)。KKC_QX × KKC_QW × 入力の数 が 21.47 億を超えないこと
#endif

// 16 ビット 2 つを 32 ビット 1 つとして読む (GCC の型による別名の仮定を外す。MSVC はその仮定をしない)
#if defined(__GNUC__)
typedef int kkc_pair32 __attribute__((may_alias));
#else
typedef int kkc_pair32;
#endif

// 重みの詰め方 (16 出力 × 入力 2 つずつ): 出力のかたまり (16 本) ごとに、入力 i, i+1 の組ごとに
// [o..o+7 の (w[i][o], w[i+1][o])] [o+8..o+15 の (w[i][o], w[i+1][o])] の 32 個。入力の数は偶数に詰める (0 で埋める)

// 行ごとに ±KKC_QX に丸める。Xq は T × inp (inp は in を偶数に)、xs は行ごとの倍率 (元の値 = Xq × xs)
static KKC_AVX2_FN void kkc_simd_quant_rows(const float* X, int T, int in, int inp, short* Xq, float* xs) {
    const __m256 absmask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    for (int t = 0; t < T; t++) {
        const float* x = X + (size_t)t * in;
        __m256 m8 = _mm256_setzero_ps();
        int i = 0;
        for (; i + 8 <= in; i += 8) m8 = _mm256_max_ps(m8, _mm256_and_ps(_mm256_loadu_ps(x + i), absmask));
        float m = 0;
        {
            float buf[8];
            _mm256_storeu_ps(buf, m8);
            for (int j = 0; j < 8; j++) m = buf[j] > m ? buf[j] : m;
            for (; i < in; i++) { const float a = x[i] < 0 ? -x[i] : x[i]; m = a > m ? a : m; }
        }
        const float s = m > 0 ? m / KKC_QX : 1.0f, inv = 1.0f / s;
        xs[t] = s;
        short* q = Xq + (size_t)t * inp;
        const __m256 iv = _mm256_set1_ps(inv);
        i = 0;
        for (; i + 16 <= in; i += 16) {
            const __m256i a = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(x + i), iv));
            const __m256i b = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(x + i + 8), iv));
            _mm256_storeu_si256((__m256i*)(q + i), _mm256_permute4x64_epi64(_mm256_packs_epi32(a, b), 0xD8));
        }
        for (; i < in; i++) {
            const float v = x[i] * inv;
            q[i] = (short)(v < 0 ? v - 0.5f : v + 0.5f);
        }
        for (; i < inp; i++) q[i] = 0;
    }
}

// Y[t][o] = (Σ_i Xq[t][i] · Wq[i][o]) · xs[t] · ws[o] + B[o]。出力は [o0, o1) (16 の倍数)。行は 6 本ずつ
static KKC_AVX2_FN void kkc_simd_lin_q(const short* Xq, const float* xs, int T, int inp, const short* Wp, const float* ws,
                                       const float* B, float* Y, int ldy, int o0, int o1) {
    const int pairs = inp / 2;
    for (int o = o0; o + 16 <= o1; o += 16) {
        const short* wt = Wp + (size_t)(o / 16) * inp * 16;
        const __m256 s0 = _mm256_loadu_ps(ws + o), s1 = _mm256_loadu_ps(ws + o + 8);
        const __m256 b0 = B ? _mm256_loadu_ps(B + o) : _mm256_setzero_ps();
        const __m256 b1 = B ? _mm256_loadu_ps(B + o + 8) : _mm256_setzero_ps();
        for (int t = 0; t < T; t += 6) {
            const int tn = T - t < 6 ? T - t : 6;
            const kkc_pair32* x[6];
            for (int r = 0; r < 6; r++) x[r] = (const kkc_pair32*)(Xq + (size_t)(t + (r < tn ? r : tn - 1)) * inp);
            __m256i a00 = _mm256_setzero_si256(), a01 = a00, a10 = a00, a11 = a00, a20 = a00, a21 = a00;
            __m256i a30 = a00, a31 = a00, a40 = a00, a41 = a00, a50 = a00, a51 = a00;
            for (int p = 0; p < pairs; p++) {
                const __m256i w0 = _mm256_loadu_si256((const __m256i*)(wt + (size_t)p * 32));
                const __m256i w1 = _mm256_loadu_si256((const __m256i*)(wt + (size_t)p * 32 + 16));
                __m256i v = _mm256_set1_epi32(x[0][p]); a00 = _mm256_add_epi32(a00, _mm256_madd_epi16(v, w0)); a01 = _mm256_add_epi32(a01, _mm256_madd_epi16(v, w1));
                v = _mm256_set1_epi32(x[1][p]); a10 = _mm256_add_epi32(a10, _mm256_madd_epi16(v, w0)); a11 = _mm256_add_epi32(a11, _mm256_madd_epi16(v, w1));
                v = _mm256_set1_epi32(x[2][p]); a20 = _mm256_add_epi32(a20, _mm256_madd_epi16(v, w0)); a21 = _mm256_add_epi32(a21, _mm256_madd_epi16(v, w1));
                v = _mm256_set1_epi32(x[3][p]); a30 = _mm256_add_epi32(a30, _mm256_madd_epi16(v, w0)); a31 = _mm256_add_epi32(a31, _mm256_madd_epi16(v, w1));
                v = _mm256_set1_epi32(x[4][p]); a40 = _mm256_add_epi32(a40, _mm256_madd_epi16(v, w0)); a41 = _mm256_add_epi32(a41, _mm256_madd_epi16(v, w1));
                v = _mm256_set1_epi32(x[5][p]); a50 = _mm256_add_epi32(a50, _mm256_madd_epi16(v, w0)); a51 = _mm256_add_epi32(a51, _mm256_madd_epi16(v, w1));
            }
            const __m256i acc[12] = {a00, a01, a10, a11, a20, a21, a30, a31, a40, a41, a50, a51};
            float* y = Y + (size_t)t * ldy + o;
            for (int r = 0; r < tn; r++) {
                const __m256 sx = _mm256_set1_ps(xs[t + r]);
                _mm256_storeu_ps(y + (size_t)r * ldy, _mm256_fmadd_ps(_mm256_cvtepi32_ps(acc[2 * r]), _mm256_mul_ps(sx, s0), b0));
                _mm256_storeu_ps(y + (size_t)r * ldy + 8, _mm256_fmadd_ps(_mm256_cvtepi32_ps(acc[2 * r + 1]), _mm256_mul_ps(sx, s1), b1));
            }
        }
    }
}

// gelu(x) = x/2 · (1 + erf(x/√2)) を 8 本ずつ。erf は Abramowitz & Stegun 7.1.26 (誤差 1.5e-7、float の丸めと同じくらい)
static KKC_AVX2_FN inline __m256 kkc_gelu8(__m256 x) {
    const __m256 sign = _mm256_set1_ps(-0.0f), one = _mm256_set1_ps(1.0f);
    const __m256 z = _mm256_mul_ps(x, _mm256_set1_ps(0.70710678118654752f));
    const __m256 a = _mm256_andnot_ps(sign, z);
    const __m256 t = _mm256_div_ps(one, _mm256_fmadd_ps(a, _mm256_set1_ps(0.3275911f), one));
    __m256 p = _mm256_set1_ps(1.061405429f);
    p = _mm256_fmadd_ps(p, t, _mm256_set1_ps(-1.453152027f));
    p = _mm256_fmadd_ps(p, t, _mm256_set1_ps(1.421413741f));
    p = _mm256_fmadd_ps(p, t, _mm256_set1_ps(-0.284496736f));
    p = _mm256_fmadd_ps(p, t, _mm256_set1_ps(0.254829592f));
    p = _mm256_mul_ps(p, t);
    const __m256 ea = _mm256_fnmadd_ps(p, kkc_exp8(_mm256_sub_ps(_mm256_setzero_ps(), _mm256_mul_ps(a, a))), one);   // erf(|z|)
    const __m256 erf = _mm256_or_ps(ea, _mm256_and_ps(z, sign));
    return _mm256_mul_ps(_mm256_mul_ps(_mm256_set1_ps(0.5f), x), _mm256_add_ps(one, erf));
}

static KKC_AVX2_FN void kkc_simd_gelu(float* v, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(v + i, kkc_gelu8(_mm256_loadu_ps(v + i)));
    if (i < n) {   // 端は 8 本に詰めて同じ式で
        float buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (size_t j = i; j < n; j++) buf[j - i] = v[j];
        _mm256_storeu_ps(buf, kkc_gelu8(_mm256_loadu_ps(buf)));
        for (size_t j = i; j < n; j++) v[j] = buf[j - i];
    }
}

// 1 つのヘッドの自己注意 (engine.cpp の encode の中の計算と同じ。dh は 8 の倍数)。sc は T 本 + 8 本の作業領域
static KKC_AVX2_FN void kkc_simd_attend(const float* qkv, int T, int d, int dh, int hh, float scale, float* sc, float* att) {
    const size_t ld = (size_t)3 * d;
    for (int t = 0; t < T; t++) {
        const float* q = qkv + (size_t)t * ld + hh * dh;
        float mx = -1e30f;
        for (int u = 0; u < T; u++) {
            const float* k = qkv + (size_t)u * ld + d + hh * dh;
            __m256 acc = _mm256_setzero_ps();
            for (int i = 0; i < dh; i += 8) acc = _mm256_fmadd_ps(_mm256_loadu_ps(q + i), _mm256_loadu_ps(k + i), acc);
            const float s = kkc_hsum8(acc) * scale;
            sc[u] = s;
            mx = s > mx ? s : mx;
        }
        // e^(s - 最大) と合計。端の 8 本未満は -1e30 で埋めて同じ式で (e^ が 0 になる)
        for (int u = T; u < ((T + 7) & ~7); u++) sc[u] = -1e30f;
        const __m256 m8 = _mm256_set1_ps(mx);
        __m256 z8 = _mm256_setzero_ps();
        for (int u = 0; u < T; u += 8) {
            const __m256 p = kkc_exp8(_mm256_sub_ps(_mm256_loadu_ps(sc + u), m8));
            _mm256_storeu_ps(sc + u, p);
            z8 = _mm256_add_ps(z8, p);
        }
        const float iz = 1.0f / kkc_hsum8(z8);
        float* o = att + (size_t)t * d + hh * dh;
        for (int i = 0; i < dh; i += 8) _mm256_storeu_ps(o + i, _mm256_setzero_ps());
        for (int u = 0; u < T; u++) {
            const __m256 p = _mm256_set1_ps(sc[u] * iz);
            const float* v = qkv + (size_t)u * ld + 2 * d + hh * dh;
            for (int i = 0; i < dh; i += 8) _mm256_storeu_ps(o + i, _mm256_fmadd_ps(p, _mm256_loadu_ps(v + i), _mm256_loadu_ps(o + i)));
        }
    }
}
