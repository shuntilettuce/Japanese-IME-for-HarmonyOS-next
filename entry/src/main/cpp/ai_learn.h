// AI学習 (設定、既定オフ) のスマホ版 (Android の jni.cpp・HarmonyOS の napi_kkc.cpp から使う)。
// 確定した文から、変換の点に上乗せする小さいモデル (engine.h の kkc_romance_*) を 1 歩ずつ育てて保存する。
// 保存の形は PC 版 (desktop/core/converter.cpp) と同じ: "SHA2" + 学んだ記録の時刻 (int64) + 記録の大きさ (int64)
// + 個数 (int32) + float の並び。スマホには確定した文の記録が無いので、時刻と大きさは 0 のまま
// 2026-11-06: 保存先はモデルごと。渡された path (…/ai_learn.bin) を …/ai_learn_<モデルの印>.bin にして使う。
// 前の ai_learn.bin は、今のモデルと大きさが合えばその名前に移して引き継ぐ。使わなくなったモデルの分は新しい 4 つまで残す
#pragma once
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "engine.h"

namespace kkc_ai {

struct State {
    std::string base;     // 渡された保存先 (…/ai_learn.bin。空ならオフ)
    std::string path;     // 今のモデルの保存先 (…/ai_learn_<印>.bin)
    long long seen = -1;  // 最後に読んだか書いたときの保存先の時刻 (無ければ -1)
};

// 学ぶ速さ (v・W・網・初めの値へ引き戻す強さ・上乗せの上限)。PC 版と同じ
inline void config(kkc_engine* e) { kkc_romance_config(e, 0.2f, 0.01f, 0.05f, 0.01f, 2.0f); }

inline long long mtime_of(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return -1;
    return static_cast<long long>(st.st_mtime) * 1000000000LL +
#if defined(__APPLE__)
           st.st_mtimespec.tv_nsec;
#else
           st.st_mtim.tv_nsec;
#endif
}

inline bool read(const std::string& path, kkc_engine* e) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4] = {};
    int64_t u = 0, b = 0;
    int32_t n = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::string(magic, 4) == "SHA2" && std::fread(&u, 8, 1, f) == 1 &&
              std::fread(&b, 8, 1, f) == 1 && std::fread(&n, 4, 1, f) == 1 && n == kkc_romance_size(e) && n > 0;
    std::vector<float> v(ok ? static_cast<size_t>(n) : 0);
    ok = ok && std::fread(v.data(), 4, v.size(), f) == v.size();
    std::fclose(f);
    if (ok) kkc_romance_set(e, v.data(), n);
    return ok;
}

inline bool write(const std::string& path, kkc_engine* e) {
    const int n = kkc_romance_size(e);
    if (n <= 0) return false;
    std::vector<float> v(static_cast<size_t>(n));
    if (kkc_romance_get(e, v.data(), n) != n) return false;
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const int64_t zero = 0;
    const int32_t n32 = n;
    bool ok = std::fwrite("SHA2", 1, 4, f) == 4 && std::fwrite(&zero, 8, 1, f) == 1 && std::fwrite(&zero, 8, 1, f) == 1 &&
              std::fwrite(&n32, 4, 1, f) == 1 && std::fwrite(v.data(), 4, v.size(), f) == v.size();
    ok = std::fclose(f) == 0 && ok;
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

inline std::string dir_of(const std::string& p) {
    const size_t k = p.find_last_of('/');
    return k == std::string::npos ? std::string(".") : p.substr(0, k);
}

// …/ai_learn.bin → …/ai_learn_<モデルの印>.bin
inline std::string model_path(const std::string& base, kkc_engine* e) {
    if (base.empty()) return base;
    std::string stem = base;
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".bin") == 0) stem.resize(stem.size() - 4);
    char tag[16];
    std::snprintf(tag, sizeof tag, "_%08x.bin", unsigned(kkc_model_tag(e)));
    return stem + tag;
}

// 保存したものの個数が今のモデルと合うか (頭だけ読む)
inline bool fits(const std::string& path, kkc_engine* e) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4] = {};
    int64_t u = 0, b = 0;
    int32_t n = 0;
    const bool ok = std::fread(magic, 1, 4, f) == 4 && std::string(magic, 4) == "SHA2" && std::fread(&u, 8, 1, f) == 1 &&
                    std::fread(&b, 8, 1, f) == 1 && std::fread(&n, 4, 1, f) == 1 && n == kkc_romance_size(e) && n > 0;
    std::fclose(f);
    return ok;
}

// フォルダの ai_learn_*.bin (と前の ai_learn.bin)
inline std::vector<std::string> model_files(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* x = readdir(d)) {
            const std::string n = x->d_name;
            if (n.size() > 13 && n.compare(0, 9, "ai_learn_") == 0 && n.compare(n.size() - 4, 4, ".bin") == 0) out.push_back(dir + "/" + n);
        }
        closedir(d);
    }
    return out;
}

// 学習のリセット: どのモデルの分も消す (base = …/ai_learn.bin)
inline void forget_all(const std::string& base) {
    unlink(base.c_str());
    for (const auto& p : model_files(dir_of(base))) unlink(p.c_str());
}

// 使っていないモデルの分は、新しいものから 4 つまで (今のモデルの分は数えない)
inline void prune(const std::string& keep) {
    auto files = model_files(dir_of(keep));
    files.erase(std::remove(files.begin(), files.end(), keep), files.end());
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) { return mtime_of(a) > mtime_of(b); });
    for (size_t k = 4; k < files.size(); k++) unlink(files[k].c_str());
}

// 保存したものが消された (学習のリセット) なら初めの値に戻す。書き換わっていたら読み直す
inline void sync(State& s, kkc_engine* e) {
    if (s.path.empty()) return;
    const long long t = mtime_of(s.path);
    if (t == s.seen) return;
    if (t < 0 || !read(s.path, e)) kkc_romance_reset(e);
    s.seen = t;
}

// オン・オフ (path が空ならオフ)。同じ保存先なら、消された・書き換わったかだけを見る
inline void set(State& s, kkc_engine* e, const std::string& base) {
    if (!e) return;
    const std::string path = model_path(base, e);   // モデルを開き直したら、同じ base でも保存先が替わる
    if (base == s.base && path == s.path) {
        sync(s, e);
        return;
    }
    s.base = base;
    s.path = path;
    s.seen = -1;
    kkc_romance_enable(e, path.empty() ? 0 : 1);
    if (path.empty()) return;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 && fits(base, e)) std::rename(base.c_str(), path.c_str());   // 前の 1 つだけの保存を引き継ぐ
    prune(path);
    kkc_romance_reset(e);
    config(e);
    s.seen = -2;   // 必ず読む
    sync(s, e);
}

// 確定した文 (左の文脈・読み・表記) から 1 歩学んで保存する。返り値 = 学ぶ前の損失 (学ばなかったら負)
inline float learn(State& s, kkc_engine* e, const std::u16string& ctx, const std::u16string& reading, const std::u16string& text) {
    if (!e || s.path.empty() || reading.empty() || text.empty() || text == reading) return -1;
    sync(s, e);
    auto p16 = [](const std::u16string& x) { return reinterpret_cast<const uint16_t*>(x.data()); };
    std::vector<uint16_t> buf(reading.size() * 40 + 256);
    if (kkc_convert(e, p16(ctx), int(ctx.size()), p16(reading), int(reading.size()), 10, 1, buf.data(), int(buf.size())) <= 0) return -1;
    float loss = 0;
    if (kkc_romance_learn(e, p16(text), int(text.size()), &loss) != 1) return -1;
    if (write(s.path, e)) s.seen = mtime_of(s.path);
    return loss;
}

}  // namespace kkc_ai
