// グライドのキーの確率のネットワーク (glide_nn.h)。shuntllim/glide/nn/train.py の Net と同じ計算
#include "glide_nn.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace glide {

namespace {
constexpr int NB = 8;
inline float gelu(float x) { return 0.5f * x * (1.0f + std::erf(x * 0.70710678f)); }
inline float sigm(float x) { return 1.0f / (1.0f + std::exp(-x)); }
inline float logsig(float x) { return x >= 0 ? -std::log1p(std::exp(-x)) : x - std::log1p(std::exp(x)); }
void layernorm(float* x, int d, const float* w, const float* b) {
    float m = 0, v = 0;
    for (int i = 0; i < d; i++) m += x[i];
    m /= float(d);
    for (int i = 0; i < d; i++) v += (x[i] - m) * (x[i] - m);
    const float r = 1.0f / std::sqrt(v / float(d) + 1e-5f);
    for (int i = 0; i < d; i++) x[i] = (x[i] - m) * r * w[i] + b[i];
}
// 内積。8 本に分けて足すとコンパイラが SIMD (NEON・AVX) にできる (浮動小数の足す順を変えてよいと言わなくても)
inline float dot(const float* a, const float* b, int n) {
    float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int i = 0;
    for (; i + 8 <= n; i += 8)
        for (int j = 0; j < 8; j++) s[j] += a[i + j] * b[i + j];
    float r = ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
    for (; i < n; i++) r += a[i] * b[i];
    return r;
}
// y[o] = b[o] + Σ W[o][i] x[i]
void linear(const float* W, const float* b, const float* x, float* y, int in, int out) {
    for (int o = 0; o < out; o++) y[o] = b[o] + dot(W + size_t(o) * in, x, in);
}
}  // namespace

bool Net::load(const void* data, size_t n) {
    d_ = 0;
    if (n < 8 || memcmp(data, "GNN1", 4) != 0) return false;
    int32_t d;
    memcpy(&d, static_cast<const char*>(data) + 4, 4);
    const size_t D = size_t(d), H = 4 * D, S = D / 8;
    const size_t per_blk = D * 7 + D + D + D + H * D + H + D * H + D + S * D + S + D * S + D;
    const size_t total = D * 8 * 3 + D + 5 * per_blk + D * D * 2 + D + D + D + 65 * D + 65;
    if (n != 8 + total * 4) return false;
    w_.resize(total);
    memcpy(w_.data(), static_cast<const char*>(data) + 8, total * 4);
    const float* p = w_.data();
    auto take = [&](size_t k) { const float* q = p; p += k; return q; };
    stem_ = take(D * 8 * 3); stemb_ = take(D);
    const int dil[5] = {1, 2, 3, 5, 8};
    for (int i = 0; i < 5; i++) {
        Blk& b = b_[i];
        b.dw = take(D * 7); b.dwb = take(D); b.lnw = take(D); b.lnb = take(D);
        b.p1 = take(H * D); b.p1b = take(H); b.p2 = take(D * H); b.p2b = take(D);
        b.s1 = take(S * D); b.s1b = take(S); b.s2 = take(D * S); b.s2b = take(D);
        b.dil = dil[i];
    }
    down_ = take(D * D * 2); downb_ = take(D); lnw_ = take(D); lnb_ = take(D); head_ = take(65 * D); headb_ = take(65);
    d_ = d;
    return true;
}

std::vector<float> Net::run(const std::vector<float>& pts, const std::vector<float>& keys) const {
    const int D = d_, H = 4 * D, S = D / 8, K = int(keys.size() / 2);
    // 64 点に取り直す (点の番号で線形補間。タッチの点はほぼ一定の間隔で来る)
    const int n = int(pts.size() / 2);
    float p[T][2];
    for (int t = 0; t < T; t++) {
        const float u = n > 1 ? float(t) * float(n - 1) / float(T - 1) : 0.0f;
        const int i = std::min(int(u), std::max(0, n - 2));
        const float f = n > 1 ? u - float(i) : 0.0f;
        for (int c = 0; c < 2; c++) p[t][c] = n > 1 ? pts[2 * i + c] * (1 - f) + pts[2 * (i + 1) + c] * f : pts[c];
    }
    // 8 次元の特徴 (train.py の features と同じ)
    std::vector<float> x(size_t(D) * T), y(size_t(D) * T);   // [D][T]
    float fe[8][T];
    float ang[T];
    for (int t = 0; t < T; t++) {
        float v[2], a[2];
        for (int c = 0; c < 2; c++) {
            v[c] = t == 0 ? p[1][c] - p[0][c] : t == T - 1 ? p[T - 1][c] - p[T - 2][c] : (p[t + 1][c] - p[t - 1][c]) / 2;
            a[c] = (t == 0 || t == T - 1) ? 0.0f : p[t + 1][c] - 2 * p[t][c] + p[t - 1][c];
            v[c] *= 16; a[c] *= 64;
        }
        const float sp = std::sqrt(v[0] * v[0] + v[1] * v[1]);
        ang[t] = std::atan2(v[1], v[0]);
        float da = 0;
        if (t > 0) {
            const float tw = 2 * float(M_PI);
            da = ang[t] - ang[t - 1] + float(M_PI);
            da = da - tw * std::floor(da / tw) - float(M_PI);
        }
        const float cu = sp > 0.05f ? std::max(-2.0f, std::min(2.0f, da / 0.5f)) : 0.0f;
        fe[0][t] = p[t][0] * 2 - 1; fe[1][t] = p[t][1] * 2 - 1; fe[2][t] = v[0]; fe[3][t] = v[1];
        fe[4][t] = a[0]; fe[5][t] = a[1]; fe[6][t] = sp; fe[7][t] = cu;
    }
    // stem: Conv1d(8, D, 3, padding 1)
    for (int o = 0; o < D; o++)
        for (int t = 0; t < T; t++) {
            float s = stemb_[o];
            for (int i = 0; i < 8; i++)
                for (int k = 0; k < 3; k++) {
                    const int tt = t + k - 1;
                    if (tt >= 0 && tt < T) s += stem_[(size_t(o) * 8 + i) * 3 + k] * fe[i][tt];
                }
            x[size_t(o) * T + t] = s;
        }
    std::vector<float> h(size_t(T) * D), hid(H), col(D), se(S), sc(D);
    for (int bi = 0; bi < 5; bi++) {
        const Blk& b = b_[bi];
        // 深さ方向の畳み込み → [T][D]
        for (int c = 0; c < D; c++)
            for (int t = 0; t < T; t++) {
                float s = b.dwb[c];
                for (int k = 0; k < 7; k++) {
                    const int tt = t + (k - 3) * b.dil;
                    if (tt >= 0 && tt < T) s += b.dw[size_t(c) * 7 + k] * x[size_t(c) * T + tt];
                }
                h[size_t(t) * D + c] = s;
            }
        std::fill(sc.begin(), sc.end(), 0.0f);
        for (int t = 0; t < T; t++) {
            float* r = &h[size_t(t) * D];
            layernorm(r, D, b.lnw, b.lnb);
            linear(b.p1, b.p1b, r, hid.data(), D, H);
            for (int j = 0; j < H; j++) hid[j] = gelu(hid[j]);
            linear(b.p2, b.p2b, hid.data(), r, H, D);
            for (int c = 0; c < D; c++) sc[c] += r[c];
        }
        for (int c = 0; c < D; c++) sc[c] /= float(T);
        linear(b.s1, b.s1b, sc.data(), se.data(), D, S);
        for (int j = 0; j < S; j++) se[j] = std::max(0.0f, se[j]);
        linear(b.s2, b.s2b, se.data(), sc.data(), S, D);
        for (int c = 0; c < D; c++) sc[c] = sigm(sc[c]);
        for (int t = 0; t < T; t++)
            for (int c = 0; c < D; c++) x[size_t(c) * T + t] += h[size_t(t) * D + c] * sc[c];
    }
    // down: Conv1d(D, D, 2, stride 2) → ln → head
    std::vector<float> phi(size_t(K) * NB * NB);
    for (int k = 0; k < K; k++)
        for (int u = 0; u < NB; u++)
            for (int v = 0; v < NB; v++)
                phi[size_t(k) * 64 + u * NB + v] = std::cos(float(M_PI) * keys[2 * k] * float(u)) * std::cos(float(M_PI) * keys[2 * k + 1] * float(v));
    std::vector<float> out(size_t(TF) * (1 + K));
    float hd[65];
    std::vector<float> z(K);
    std::vector<float> pair(2 * size_t(D));
    for (int t = 0; t < TF; t++) {
        for (int i = 0; i < D; i++) { pair[2 * i] = x[size_t(i) * T + 2 * t]; pair[2 * i + 1] = x[size_t(i) * T + 2 * t + 1]; }
        for (int o = 0; o < D; o++) col[o] = downb_[o] + dot(down_ + size_t(o) * D * 2, pair.data(), 2 * D);
        layernorm(col.data(), D, lnw_, lnb_);
        linear(head_, headb_, col.data(), hd, D, 65);
        float mx = -1e30f;
        for (int k = 0; k < K; k++) {
            float s = 0;
            for (int j = 0; j < 64; j++) s += hd[1 + j] * phi[size_t(k) * 64 + j];
            z[k] = s; mx = std::max(mx, s);
        }
        float se2 = 0;
        for (int k = 0; k < K; k++) se2 += std::exp(z[k] - mx);
        const float lse = mx + std::log(se2), ll = logsig(hd[0]);
        float* r = &out[size_t(t) * (1 + K)];
        r[0] = logsig(-hd[0]);
        for (int k = 0; k < K; k++) r[1 + k] = ll + z[k] - lse;
    }
    return out;
}

}  // namespace glide
