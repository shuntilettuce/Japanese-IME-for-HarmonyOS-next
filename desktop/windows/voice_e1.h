// shuntear E1 の推論 (C++。学習側の Feat + Model と同じ計算)。2026-11-16。声の入力の窓 (voice_app.cpp) が使う
//   E1 e; e.load(bytes, size);  std::vector<float> lp; int T = e.run(pcm16k, n, lp);   // lp = T × ncol (空白 + かなの列の対数確率)
// 重みは voice_e1.bin (float16)。メルの表と窓も同じファイルに入っている
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

struct E1 {
    int d = 0, L = 0, H = 0, K = 0, nk = 0, nf = 0, nm = 0, nw = 0, nc = 0;
    std::vector<float> win, fb, W;   // W = 重み (書き出しの順に並べた float32)
    std::vector<int> col;
    struct Lin { const float *w, *b; int out, in; };
    struct Ln { const float *w, *b; };
    struct Layer { Ln f1n; Lin f1a, f1b; Ln an; Lin qkv, ao; Ln cn; Lin pw1; const float *dw, *dwb, *bnw, *bnb, *bnm, *bnv; Lin pw2; Ln f2n; Lin f2a, f2b; Ln fin; };
    const float *c1w, *c1b, *c2w, *c2b; Lin proj, kh; std::vector<Layer> layers;

    static float h2f(uint16_t h) {
        uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023, f;
        if (e == 0) { float v = std::ldexp(float(m), -24); return s ? -v : v; }
        if (e == 31) f = (s << 31) | 0x7f800000 | (m << 13);
        else f = (s << 31) | ((e + 112) << 23) | (m << 13);
        float v; std::memcpy(&v, &f, 4); return v;
    }
    bool load(const uint8_t* p, size_t n) {
        if (n < 40 || std::memcmp(p, "SHE1", 4)) return false;
        int32_t hd[9]; std::memcpy(hd, p + 4, sizeof(hd));
        d = hd[0]; L = hd[1]; H = hd[2]; K = hd[3]; nk = hd[4]; nf = hd[5]; nm = hd[6]; nw = hd[7]; nc = hd[8];
        size_t o = 4 + sizeof(hd);
        win.resize(size_t(nw)); std::memcpy(win.data(), p + o, win.size() * 4); o += win.size() * 4;
        fb.resize(size_t(nf) * nm); std::memcpy(fb.data(), p + o, fb.size() * 4); o += fb.size() * 4;
        col.resize(size_t(nc)); std::memcpy(col.data(), p + o, col.size() * 4); o += col.size() * 4;
        const size_t nw16 = (n - o) / 2;
        W.resize(nw16);
        for (size_t i = 0; i < nw16; i++) { uint16_t h; std::memcpy(&h, p + o + 2 * i, 2); W[i] = h2f(h); }
        size_t q = 0;
        auto take = [&](size_t k) { const float* r = W.data() + q; q += k; return r; };
        auto lin = [&](int out, int in) { Lin l; l.out = out; l.in = in; l.w = take(size_t(out) * in); l.b = take(size_t(out)); return l; };
        auto ln = [&]() { Ln l; l.w = take(size_t(d)); l.b = take(size_t(d)); return l; };
        const int ch = 128, F2 = ((nm - 3) / 2 + 1 - 3) / 2 + 1;
        c1w = take(size_t(ch) * 9); c1b = take(size_t(ch)); c2w = take(size_t(ch) * ch * 9); c2b = take(size_t(ch));
        proj = lin(d, ch * F2);
        layers.resize(size_t(L));
        for (auto& y : layers) {
            y.f1n = ln(); y.f1a = lin(4 * d, d); y.f1b = lin(d, 4 * d);
            y.f2n = ln(); y.f2a = lin(4 * d, d); y.f2b = lin(d, 4 * d);   // 書き出しは ffn1・ffn2 の順に続けて並べた
            y.an = ln(); y.qkv = lin(3 * d, d); y.ao = lin(d, d);
            y.cn = ln(); y.pw1 = lin(2 * d, d); y.dw = take(size_t(d) * K); y.dwb = take(size_t(d));
            y.bnw = take(size_t(d)); y.bnb = take(size_t(d)); y.bnm = take(size_t(d)); y.bnv = take(size_t(d));
            y.pw2 = lin(d, d); y.fin = ln();
        }
        kh = lin(nk, d);
        return q == W.size();
    }
    static void linear(const Lin& l, const float* x, int T, float* y) {
        for (int t = 0; t < T; t++) {
            const float* xt = x + size_t(t) * l.in;
            float* yt = y + size_t(t) * l.out;
            for (int o = 0; o < l.out; o++) {
                const float* w = l.w + size_t(o) * l.in;
                float s = l.b[o];
                for (int i = 0; i < l.in; i++) s += w[i] * xt[i];
                yt[o] = s;
            }
        }
    }
    void lnorm(const Ln& l, const float* x, int T, float* y) const {
        for (int t = 0; t < T; t++) {
            const float* v = x + size_t(t) * d; float* o = y + size_t(t) * d;
            float m = 0, s = 0;
            for (int i = 0; i < d; i++) m += v[i];
            m /= d;
            for (int i = 0; i < d; i++) { float z = v[i] - m; s += z * z; }
            const float r = 1.0f / std::sqrt(s / d + 1e-5f);
            for (int i = 0; i < d; i++) o[i] = (v[i] - m) * r * l.w[i] + l.b[i];
        }
    }
    static float silu(float x) { return x / (1.0f + std::exp(-x)); }
    void ffn(const Ln& n, const Lin& a, const Lin& b, std::vector<float>& x, int T) const {
        std::vector<float> h(size_t(T) * d), u(size_t(T) * 4 * d), o(size_t(T) * d);
        lnorm(n, x.data(), T, h.data()); linear(a, h.data(), T, u.data());
        for (auto& v : u) v = silu(v);
        linear(b, u.data(), T, o.data());
        for (size_t i = 0; i < x.size(); i++) x[i] += 0.5f * o[i];
    }
    // pcm: 16 kHz・-1〜1。返り値 = フレームの数 (40 ms)
    int run(const float* pcm, int n, std::vector<float>& lp) const {
        // メル (torchaudio: center = 反射で 200 ずつ足す、hann、パワー)
        const int hop = 160, nfft = 400, Tm = n / hop + 1;
        std::vector<float> pad(size_t(n) + nfft);
        for (int i = 0; i < n + nfft; i++) {
            int j = i - nfft / 2;
            if (j < 0) j = -j;
            if (j >= n) j = 2 * (n - 1) - j;
            pad[size_t(i)] = n > 0 ? pcm[std::max(0, std::min(n - 1, j))] : 0.f;
        }
        static std::vector<float> cs, sn;
        if (cs.empty()) {
            cs.resize(size_t(nf) * nfft); sn.resize(size_t(nf) * nfft);
            for (int k = 0; k < nf; k++) for (int i = 0; i < nfft; i++) { double a = 2 * M_PI * k * i / nfft; cs[size_t(k) * nfft + i] = float(std::cos(a)); sn[size_t(k) * nfft + i] = float(std::sin(a)); }
        }
        std::vector<float> x(size_t(Tm) * nm), fr(nfft + 0), pw(nf + 0);   // + 0: 関数の宣言と読まれないように
        for (int t = 0; t < Tm; t++) {
            for (int i = 0; i < nfft; i++) fr[size_t(i)] = pad[size_t(t) * hop + i] * win[size_t(i)];
            for (int k = 0; k < nf; k++) {
                float re = 0, im = 0; const float *c = &cs[size_t(k) * nfft], *s = &sn[size_t(k) * nfft];
                for (int i = 0; i < nfft; i++) { re += fr[size_t(i)] * c[i]; im += fr[size_t(i)] * s[i]; }
                pw[size_t(k)] = re * re + im * im;
            }
            for (int m = 0; m < nm; m++) { float s = 0; for (int k = 0; k < nf; k++) s += pw[size_t(k)] * fb[size_t(k) * nm + m]; x[size_t(t) * nm + m] = std::log(s + 1e-6f); }
        }
        for (int m = 0; m < nm; m++) {   // 発話ごとに平均 0・分散 1
            double mu = 0, v = 0;
            for (int t = 0; t < Tm; t++) mu += x[size_t(t) * nm + m];
            mu /= Tm;
            for (int t = 0; t < Tm; t++) { double z = x[size_t(t) * nm + m] - mu; v += z * z; }
            const float sdc = float(std::sqrt(std::max(v / Tm, 1e-5)));   // train.py: 分散を 1e-5 で下を切ってから平方根
            for (int t = 0; t < Tm; t++) x[size_t(t) * nm + m] = float((x[size_t(t) * nm + m] - mu) / sdc);
        }
        // 畳み込み 2 回 (3×3・幅 2・ReLU)
        const int ch = 128, T1 = (Tm - 3) / 2 + 1, F1 = (nm - 3) / 2 + 1, T2 = (T1 - 3) / 2 + 1, F2 = (F1 - 3) / 2 + 1;
        if (T2 < 1) return 0;
        std::vector<float> a1(size_t(ch) * T1 * F1), a2(size_t(T2) * ch * F2);
        for (int c = 0; c < ch; c++) for (int t = 0; t < T1; t++) for (int f = 0; f < F1; f++) {
            float s = c1b[c];
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) s += c1w[size_t(c) * 9 + i * 3 + j] * x[size_t(2 * t + i) * nm + 2 * f + j];
            a1[(size_t(c) * T1 + t) * F1 + f] = s > 0 ? s : 0;
        }
        for (int c = 0; c < ch; c++) for (int t = 0; t < T2; t++) for (int f = 0; f < F2; f++) {
            float s = c2b[c];
            for (int c2 = 0; c2 < ch; c2++) {
                const float* w = c2w + (size_t(c) * ch + c2) * 9; const float* in = &a1[size_t(c2) * T1 * F1];
                for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) s += w[i * 3 + j] * in[size_t(2 * t + i) * F1 + 2 * f + j];
            }
            a2[(size_t(t) * ch + c) * F2 + f] = s > 0 ? s : 0;   // (T, C, F) の順に並べる = permute(0,2,1,3).flatten
        }
        const int T = T2;
        std::vector<float> h(size_t(T) * d);
        linear(proj, a2.data(), T, h.data());
        std::vector<float> tmp(size_t(T) * d), qkv(size_t(T) * 3 * d), att(size_t(T) * d), g(size_t(T) * 2 * d), sc(T + 0);
        const int dh = d / H; const float scale = 1.0f / std::sqrt(float(dh));
        for (const Layer& y : layers) {
            ffn(y.f1n, y.f1a, y.f1b, h, T);
            lnorm(y.an, h.data(), T, tmp.data()); linear(y.qkv, tmp.data(), T, qkv.data());
            for (int hh = 0; hh < H; hh++) for (int t = 0; t < T; t++) {
                const float* q = &qkv[size_t(t) * 3 * d + hh * dh]; float mx = -1e30f;
                for (int u = 0; u < T; u++) {
                    const float* k = &qkv[size_t(u) * 3 * d + d + hh * dh]; float s = 0;
                    for (int i = 0; i < dh; i++) s += q[i] * k[i];
                    sc[size_t(u)] = s * scale; mx = std::max(mx, sc[size_t(u)]);
                }
                float z = 0; for (int u = 0; u < T; u++) { sc[size_t(u)] = std::exp(sc[size_t(u)] - mx); z += sc[size_t(u)]; }
                float* o = &att[size_t(t) * d + hh * dh]; for (int i = 0; i < dh; i++) o[i] = 0;
                for (int u = 0; u < T; u++) { const float p = sc[size_t(u)] / z; const float* v = &qkv[size_t(u) * 3 * d + 2 * d + hh * dh]; for (int i = 0; i < dh; i++) o[i] += p * v[i]; }
            }
            linear(y.ao, att.data(), T, tmp.data());
            for (size_t i = 0; i < h.size(); i++) h[i] += tmp[i];
            // 畳み込みの部: 正規化 → 点ごと 2d → GLU → 深さごとの畳み込み → BN → SiLU → 点ごと
            lnorm(y.cn, h.data(), T, tmp.data()); linear(y.pw1, tmp.data(), T, g.data());
            for (int t = 0; t < T; t++) for (int c = 0; c < d; c++) {
                const float a = g[size_t(t) * 2 * d + c], b = g[size_t(t) * 2 * d + d + c];
                att[size_t(t) * d + c] = a / (1.0f + std::exp(-b));
            }
            const int P = (K - 1) / 2;
            for (int t = 0; t < T; t++) for (int c = 0; c < d; c++) {
                float s = y.dwb[c];
                for (int k = 0; k < K; k++) { const int u = t + k - P; if (u >= 0 && u < T) s += y.dw[size_t(c) * K + k] * att[size_t(u) * d + c]; }
                s = (s - y.bnm[c]) / std::sqrt(y.bnv[c] + 1e-5f) * y.bnw[c] + y.bnb[c];
                tmp[size_t(t) * d + c] = silu(s);
            }
            linear(y.pw2, tmp.data(), T, att.data());
            for (size_t i = 0; i < h.size(); i++) h[i] += att[i];
            ffn(y.f2n, y.f2a, y.f2b, h, T);
            lnorm(y.fin, h.data(), T, tmp.data()); h.swap(tmp);
        }
        std::vector<float> lg(size_t(T) * nk);
        linear(kh, h.data(), T, lg.data());
        lp.assign(size_t(T) * nc, -30.0f);
        for (int t = 0; t < T; t++) {
            const float* r = &lg[size_t(t) * nk]; float mx = -1e30f;
            for (int i = 0; i < nk; i++) mx = std::max(mx, r[i]);
            double z = 0; for (int i = 0; i < nk; i++) z += std::exp(double(r[i] - mx));
            const float lz = mx + float(std::log(z));
            for (int j = 0; j < nc; j++) if (col[size_t(j)] >= 0) lp[size_t(t) * nc + j] = r[col[size_t(j)]] - lz;
        }
        return T;
    }
};
