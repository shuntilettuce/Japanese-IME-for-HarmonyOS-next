// HarmonyOS (ArkTS) から変換エンジン (kkc/engine/engine.cpp) を呼ぶ NAPI の窓口。モジュール名 libkkc.so。
//
//   import kkc from 'libkkc.so';
//   await kkc.loadAsync(context.resourceManager, context.filesDir, 4)   // 辞書とモデルを開く (裏のスレッドで)
//   await kkc.convertAsync('左の文脈', 'よみ', 30)                         // 候補と 1 位の区切り
//
// IME の拡張はメモリの上限が厳しいので、辞書とモデル (合わせて 100MB 超) を JS に読み込まない。
// rawfile の kkc_lex.bin / kkc_model.bin は HAP の中に圧縮せずに入っているので、HAP のファイルのその位置を直接 mmap する
// (端末に写さない。必要なページだけが読まれ、他のアプリと同じく OS が管理する)。
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "napi/native_api.h"
#include "rawfile/raw_file.h"
#include "rawfile/raw_file_manager.h"

#include "ai_learn.h"
#include "engine.h"

namespace {

struct Mapped {
    const void* p = nullptr;   // ファイルの先頭
    size_t n = 0;
    void* base = nullptr;      // mmap した範囲 (ページの境目から)
    size_t len = 0;
};

const char* LEX = "kkc_lex.bin";     // rawfile の名前 (Mozc の資産と区別する)
const char* MODEL = "kkc_model.bin";
// 変換の方式 (engine.h の use_model)。2 = 段階式: 辞書が迷っていないときは辞書だけで決め、迷ったときだけモデルを通す
// (正解率はほぼそのままで、普段の入力ではモデルの計算が半分以下になる)
const int KKC_MODE = 2;
kkc_engine* g_engine = nullptr;
bool g_suggest = true;   // 提案を分けて出す (設定「提案を分けて表示する」。読み込む前に決まっていることがある)
std::mutex g_mu;   // エンジンは同時に 2 つの変換をできないので、呼び出しを 1 つずつにする
Mapped g_lex, g_model;
kkc_ai::State g_ai;    // AI学習 (設定、既定オフ)
std::string g_ai_path;  // その保存先 (エンジンを開き直したら入れ直す)

std::string str_arg(napi_env env, napi_value v) {
    size_t n = 0;
    napi_get_value_string_utf8(env, v, nullptr, 0, &n);
    std::string s(n, '\0');
    napi_get_value_string_utf8(env, v, s.data(), n + 1, &n);
    return s;
}

std::u16string u16_arg(napi_env env, napi_value v) {
    size_t n = 0;
    napi_get_value_string_utf16(env, v, nullptr, 0, &n);
    std::u16string s(n, u'\0');
    napi_get_value_string_utf16(env, v, s.data(), n + 1, &n);
    return s;
}

napi_value boolean(napi_env env, bool b) {
    napi_value r;
    napi_get_boolean(env, b, &r);
    return r;
}

void unmap(Mapped& m) {
    if (m.base) munmap(m.base, m.len);
    m = Mapped();
}

// rawfile の name を、HAP のファイルの中の位置のまま mmap する (HAP の中で圧縮されていない rawfile だけ。
// hvigor は rawfile を圧縮せずに入れる)。mmap の位置はページの境目にそろえる必要があるので、手前の境目から写して先頭をずらす
bool map_raw(NativeResourceManager* mgr, const char* name, Mapped& m) {
    RawFile* rf = OH_ResourceManager_OpenRawFile(mgr, name);
    if (!rf) return false;
    RawFileDescriptor d;
    bool ok = OH_ResourceManager_GetRawFileDescriptorData(rf, &d);
    OH_ResourceManager_CloseRawFile(rf);
    if (!ok) return false;
    // 辞書・モデルの中の数の配列を 4 バイトの境目で読むので、HAP の中の位置もそろっていること
    if (d.fd >= 0 && d.length > 0 && d.start % 4 == 0) {
        const long page = sysconf(_SC_PAGESIZE);
        const long off = d.start - d.start % page;
        const size_t len = size_t(d.length + (d.start - off));
        void* p = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, d.fd, off);
        if (p != MAP_FAILED) {
            m.base = p;
            m.len = len;
            m.p = static_cast<const char*>(p) + (d.start - off);
            m.n = size_t(d.length);
        }
    }
    OH_ResourceManager_ReleaseRawFileDescriptorData(&d);   // fd を閉じても mmap は残る
    return m.p != nullptr;
}

struct LoadJob {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    NativeResourceManager* mgr = nullptr;
    std::string dir;
    int32_t threads = 4;
    bool ok = false;
};

// 辞書とモデルを開く (裏のスレッドで)。Promise<boolean>
napi_value LoadAsync(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    auto* job = new LoadJob();
    if (argc >= 2) {
        job->mgr = OH_ResourceManager_InitNativeResourceManager(env, argv[0]);
        job->dir = str_arg(env, argv[1]);
    }
    if (argc >= 3) napi_get_value_int32(env, argv[2], &job->threads);
    napi_value promise, name;
    napi_create_promise(env, &job->deferred, &promise);
    napi_create_string_utf8(env, "kkcLoad", NAPI_AUTO_LENGTH, &name);
    napi_create_async_work(
        env, nullptr, name,
        [](napi_env, void* data) {
            auto* j = static_cast<LoadJob*>(data);
            if (!j->mgr) return;
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_engine) { kkc_close(g_engine); g_engine = nullptr; }
            unmap(g_lex);
            unmap(g_model);
            if (!map_raw(j->mgr, LEX, g_lex) || !map_raw(j->mgr, MODEL, g_model)) return;
            // 1.6.1 までは初回に filesDir へ写して使っていた。その写し (100MB 超) は要らないので消す
            unlink((j->dir + "/" + LEX).c_str());
            unlink((j->dir + "/" + MODEL).c_str());
            g_engine = kkc_open(g_lex.p, g_lex.n, g_model.p, g_model.n);
            if (g_engine) kkc_set_threads(g_engine, j->threads);
            if (g_engine) kkc_set_suggest(g_engine, g_suggest ? 1 : 0);   // 打ち間違いの直し・日付・住所は候補に混ぜず提案 (sugg) に
            g_ai = kkc_ai::State();   // 新しいエンジンに AI学習を入れ直す
            if (g_engine) kkc_ai::set(g_ai, g_engine, g_ai_path);
            j->ok = g_engine != nullptr;
        },
        [](napi_env env, napi_status, void* data) {
            auto* j = static_cast<LoadJob*>(data);
            if (j->mgr) OH_ResourceManager_ReleaseNativeResourceManager(j->mgr);
            napi_resolve_deferred(env, j->deferred, boolean(env, j->ok));
            napi_delete_async_work(env, j->work);
            delete j;
        },
        job, &job->work);
    napi_queue_async_work(env, job->work);
    return promise;
}

// 変換 (呼んだスレッドで)。{cands, ends, lens} (ConvertAsync の結果から時間を除いたもの)。文節を選ぶとき・語を作れるかを確かめるときに使う
napi_value Convert(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    napi_value obj, arr, ends, lens;
    napi_create_object(env, &obj);
    napi_create_array(env, &arr);
    napi_create_array(env, &ends);
    napi_create_array(env, &lens);
    napi_set_named_property(env, obj, "cands", arr);
    napi_set_named_property(env, obj, "ends", ends);
    napi_set_named_property(env, obj, "lens", lens);
    if (!g_engine || argc < 2) return obj;
    std::u16string ctx = u16_arg(env, argv[0]), kana = u16_arg(env, argv[1]);
    if (kana.empty()) return obj;
    int32_t maxout = 10;
    if (argc >= 3) napi_get_value_int32(env, argv[2], &maxout);
    std::vector<uint16_t> out(1 << 15);
    std::vector<int32_t> e(512), l(512);
    int r, m;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        r = kkc_convert(g_engine, reinterpret_cast<const uint16_t*>(ctx.data()), int(ctx.size()),
                        reinterpret_cast<const uint16_t*>(kana.data()), int(kana.size()), maxout, KKC_MODE, out.data(), int(out.size()));
        m = kkc_last_segments(g_engine, e.data(), l.data(), 512);
    }
    size_t st = 0;
    for (int i = 0; i < r; i++) {
        size_t en = st;
        while (out[en]) en++;
        napi_value s;
        napi_create_string_utf16(env, reinterpret_cast<const char16_t*>(out.data() + st), en - st, &s);
        napi_set_element(env, arr, uint32_t(i), s);
        st = en + 1;
    }
    for (int i = 0; i < m; i++) {
        napi_value a, b;
        napi_create_int32(env, e[i], &a);
        napi_create_int32(env, l[i], &b);
        napi_set_element(env, ends, uint32_t(i), a);
        napi_set_element(env, lens, uint32_t(i), b);
    }
    return obj;
}

// 上位の経路を 1 本 1 文字列に ("読みの終わり\1表記\1読みの終わり\1表記…")。kkc_convert のすぐ後に同じスレッドで呼ぶ
std::vector<std::u16string> read_paths(kkc_engine* e) {
    constexpr int PM = 30, WC = 1024, TC = 4096;
    std::vector<double> costs(PM);
    std::vector<int32_t> cnt(PM), pe(WC), pl(WC);
    std::vector<uint16_t> tx(TC);
    const int np = kkc_last_paths(e, PM, costs.data(), cnt.data(), pe.data(), pl.data(), WC, tx.data(), TC);
    std::vector<std::u16string> out;
    size_t wi = 0, ti = 0;
    for (int p = 0; p < np; p++) {
        std::u16string s;
        for (int k = 0; k < cnt[size_t(p)]; k++, wi++) {
            if (k) s += u'\1';
            for (char c : std::to_string(pe[wi])) s += char16_t(c);
            s += u'\1';
            s.append(reinterpret_cast<const char16_t*>(tx.data() + ti), size_t(pl[wi]));
            ti += size_t(pl[wi]);
        }
        out.push_back(std::move(s));
    }
    return out;
}

// 裏のスレッドで変換して Promise で返す (UI のスレッドを止めない)。古い打鍵の結果は呼ぶ側で捨てる
struct Job {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::u16string ctx, kana;
    int32_t maxout = 10;
    std::vector<std::u16string> res;
    std::vector<int32_t> ends, lens;   // 1 位の候補の語の区切り
    std::vector<std::u16string> paths; // 上位の経路 (read_paths)
    std::vector<std::u16string> sugg;  // 提案 (kkc_last_suggest)
    std::vector<int32_t> sugg_kinds;
    std::chrono::steady_clock::time_point queued, started, done;   // 待ち時間の切り分け用
};

napi_value ConvertAsync(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    auto* job = new Job();
    if (argc >= 2) { job->ctx = u16_arg(env, argv[0]); job->kana = u16_arg(env, argv[1]); }
    if (argc >= 3) napi_get_value_int32(env, argv[2], &job->maxout);
    napi_value promise, name;
    napi_create_promise(env, &job->deferred, &promise);
    job->queued = std::chrono::steady_clock::now();
    napi_create_string_utf8(env, "kkcConvert", NAPI_AUTO_LENGTH, &name);
    napi_create_async_work(
        env, nullptr, name,
        [](napi_env, void* data) {
            auto* j = static_cast<Job*>(data);
            j->started = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(g_mu);
            j->done = j->started;
            if (!g_engine || j->kana.empty()) return;
            std::vector<uint16_t> out(1 << 15);
            int r = kkc_convert(g_engine, reinterpret_cast<const uint16_t*>(j->ctx.data()), int(j->ctx.size()),
                                reinterpret_cast<const uint16_t*>(j->kana.data()), int(j->kana.size()), j->maxout, KKC_MODE,
                                out.data(), int(out.size()));
            size_t st = 0;
            for (int i = 0; i < r; i++) {
                size_t e = st;
                while (out[e]) e++;
                j->res.emplace_back(reinterpret_cast<const char16_t*>(out.data() + st), e - st);
                st = e + 1;
            }
            j->ends.resize(512);
            j->lens.resize(512);
            int m = kkc_last_segments(g_engine, j->ends.data(), j->lens.data(), 512);
            j->ends.resize(m > 0 ? m : 0);
            j->lens.resize(m > 0 ? m : 0);
            j->paths = read_paths(g_engine);
            std::vector<uint16_t> sb(4096);
            int32_t kinds[16];
            int ns = kkc_last_suggest(g_engine, sb.data(), int(sb.size()), kinds, 16);
            size_t ss = 0;
            for (int i = 0; i < ns; i++) {
                size_t e = ss;
                while (sb[e]) e++;
                j->sugg.emplace_back(reinterpret_cast<const char16_t*>(sb.data() + ss), e - ss);
                j->sugg_kinds.push_back(kinds[i]);
                ss = e + 1;
            }
            j->done = std::chrono::steady_clock::now();
        },
        [](napi_env env, napi_status, void* data) {
            auto* j = static_cast<Job*>(data);
            napi_value arr, obj, ends, lens;
            napi_create_array(env, &arr);
            for (uint32_t i = 0; i < j->res.size(); i++) {
                napi_value s;
                napi_create_string_utf16(env, j->res[i].data(), j->res[i].size(), &s);
                napi_set_element(env, arr, i, s);
            }
            napi_create_array(env, &ends);
            napi_create_array(env, &lens);
            for (uint32_t i = 0; i < j->ends.size(); i++) {
                napi_value a, b;
                napi_create_int32(env, j->ends[i], &a);
                napi_create_int32(env, j->lens[i], &b);
                napi_set_element(env, ends, i, a);
                napi_set_element(env, lens, i, b);
            }
            napi_create_object(env, &obj);
            napi_set_named_property(env, obj, "cands", arr);
            napi_set_named_property(env, obj, "ends", ends);
            napi_set_named_property(env, obj, "lens", lens);
            napi_value pa;
            napi_create_array(env, &pa);
            for (uint32_t i = 0; i < j->paths.size(); i++) {
                napi_value s;
                napi_create_string_utf16(env, j->paths[i].data(), j->paths[i].size(), &s);
                napi_set_element(env, pa, i, s);
            }
            napi_set_named_property(env, obj, "paths", pa);
            napi_value sg, sk;
            napi_create_array(env, &sg);
            napi_create_array(env, &sk);
            for (uint32_t i = 0; i < j->sugg.size(); i++) {
                napi_value s, k;
                napi_create_string_utf16(env, j->sugg[i].data(), j->sugg[i].size(), &s);
                napi_create_int32(env, j->sugg_kinds[i], &k);
                napi_set_element(env, sg, i, s);
                napi_set_element(env, sk, i, k);
            }
            napi_set_named_property(env, obj, "sugg", sg);
            napi_set_named_property(env, obj, "suggKinds", sk);
            // 待ち時間の内訳 (ms): 裏のスレッドの順番待ち / 変換 / 変換が終わってから JS に届くまで
            auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            napi_value w1, w2, w3;
            auto now = std::chrono::steady_clock::now();
            napi_create_double(env, ms(j->queued, j->started), &w1);
            napi_create_double(env, ms(j->started, j->done), &w2);
            napi_create_double(env, ms(j->done, now), &w3);
            napi_set_named_property(env, obj, "waitMs", w1);
            napi_set_named_property(env, obj, "runMs", w2);
            napi_set_named_property(env, obj, "deliverMs", w3);
            napi_resolve_deferred(env, j->deferred, obj);
            napi_delete_async_work(env, j->work);
            delete j;
        },
        job, &job->work);
    napi_queue_async_work(env, job->work);
    return promise;
}

// 提案を分けて出すか (オフなら日付・住所・打ち間違いの直しは今までどおり候補に混ぜる)
napi_value SetSuggest(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    bool on = true;
    if (argc >= 1) napi_get_value_bool(env, argv[0], &on);
    std::lock_guard<std::mutex> lk(g_mu);
    g_suggest = on;
    if (g_engine) kkc_set_suggest(g_engine, on ? 1 : 0);
    napi_value r;
    napi_get_boolean(env, on, &r);
    return r;
}

// 予測: 読みが prefix で始まり、あと maxExtra 字までの辞書の語を、よく使う順に最大 maxOut 個 (辞書を読むだけなので、変換中でも待たない)
napi_value Complete(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    napi_value arr;
    napi_create_array(env, &arr);
    if (!g_engine || argc < 1) return arr;
    std::u16string prefix = u16_arg(env, argv[0]);
    int32_t maxExtra = 6, maxOut = 3;
    if (argc >= 2) napi_get_value_int32(env, argv[1], &maxExtra);
    if (argc >= 3) napi_get_value_int32(env, argv[2], &maxOut);
    std::vector<uint16_t> out(1 << 12);
    int r = kkc_complete(g_engine, reinterpret_cast<const uint16_t*>(prefix.data()), int(prefix.size()), maxExtra, maxOut,
                         out.data(), int(out.size()));
    size_t st = 0;
    for (int i = 0; i < r; i++) {
        size_t e = st;
        while (out[e]) e++;
        napi_value s;
        napi_create_string_utf16(env, reinterpret_cast<const char16_t*>(out.data() + st), e - st, &s);
        napi_set_element(env, arr, uint32_t(i), s);
        st = e + 1;
    }
    return arr;
}

// ユーザー辞書を入れ直す。forms は [読み, 表記, 代表語の読み, 代表語の表記] を 4 つずつ並べた配列 (AiConverter.userForms)。
// 入った形の数を返す (代表語が辞書に無い形は入らない)。変換と同時には触れないので、変換中なら終わるまで待つ
napi_value SetUserWords(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    napi_value res;
    int32_t bonus = 800, added = 0;
    if (argc >= 2) napi_get_value_int32(env, argv[1], &bonus);
    std::vector<std::u16string> f;
    uint32_t n = 0;
    if (argc >= 1) napi_get_array_length(env, argv[0], &n);
    for (uint32_t i = 0; i < n; i++) {
        napi_value v;
        napi_get_element(env, argv[0], i, &v);
        f.push_back(u16_arg(env, v));
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_engine) {
        kkc_user_clear(g_engine);
        auto p = [](const std::u16string& x) { return reinterpret_cast<const uint16_t*>(x.data()); };
        const std::u16string neko = u"ねこ", cat = u"猫";
        for (size_t i = 0; i + 3 < f.size(); i += 4) {
            int ok = kkc_user_add_like(g_engine, p(f[i]), int(f[i].size()), p(f[i + 1]), int(f[i + 1].size()),
                                       p(f[i + 2]), int(f[i + 2].size()), p(f[i + 3]), int(f[i + 3].size()), bonus);
            // 代表語がその語そのもの (学習で選んだ語) で辞書に無いときは、名詞の代表語で入れる (PC 版と同じ)
            if (!ok && f[i] == f[i + 2] && f[i + 1] == f[i + 3])
                ok = kkc_user_add_like(g_engine, p(f[i]), int(f[i].size()), p(f[i + 1]), int(f[i + 1].size()),
                                       p(neko), int(neko.size()), p(cat), int(cat.size()), bonus);
            added += ok;
        }
    }
    napi_create_int32(env, added, &res);
    return res;
}

// 直前の変換の時間の内訳 (ミリ秒): [網, 下書き, エンコーダ, 区間と語, 上位k]
napi_value Times(napi_env env, napi_callback_info) {
    napi_value arr;
    napi_create_array(env, &arr);
    double t[5] = {0, 0, 0, 0, 0};
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_engine) kkc_last_times(g_engine, t);
    for (uint32_t i = 0; i < 5; i++) {
        napi_value v;
        napi_create_double(env, t[i], &v);
        napi_set_element(env, arr, i, v);
    }
    return arr;
}

// AI学習 (設定、既定オフ) のオン・オフ。path = 育てた中身の保存先 (空ならオフ)。同じ保存先なら、学習のリセットで消されたかだけを見る
napi_value SetAiLearn(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string path = argc >= 1 ? str_arg(env, argv[0]) : std::string();
    std::lock_guard<std::mutex> lk(g_mu);
    g_ai_path = path;   // 読み込む前・開き直した後にも入れる
    if (g_engine) kkc_ai::set(g_ai, g_engine, path);
    return boolean(env, g_engine != nullptr);
}

struct AiLearnJob {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::u16string ctx, reading, text;
    double loss = -1;
};

// 確定した文 (左の文脈・読み・表記) から 1 歩学んで保存する (裏のスレッドで)。Promise<number> = 学ぶ前の損失 (学ばなかったら負)
napi_value AiLearnAsync(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    auto* job = new AiLearnJob();
    if (argc >= 3) {
        job->ctx = u16_arg(env, argv[0]);
        job->reading = u16_arg(env, argv[1]);
        job->text = u16_arg(env, argv[2]);
    }
    napi_value promise, name;
    napi_create_promise(env, &job->deferred, &promise);
    napi_create_string_utf8(env, "kkcAiLearn", NAPI_AUTO_LENGTH, &name);
    napi_create_async_work(
        env, nullptr, name,
        [](napi_env, void* data) {
            auto* j = static_cast<AiLearnJob*>(data);
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_engine) j->loss = kkc_ai::learn(g_ai, g_engine, j->ctx, j->reading, j->text);
        },
        [](napi_env env, napi_status, void* data) {
            auto* j = static_cast<AiLearnJob*>(data);
            napi_value v;
            napi_create_double(env, j->loss, &v);
            napi_resolve_deferred(env, j->deferred, v);
            napi_delete_async_work(env, j->work);
            delete j;
        },
        job, &job->work);
    napi_queue_async_work(env, job->work);
    return promise;
}

// AI学習で育てた中身を消す (学習のリセット)。path = 保存先 (オフでも消す)。使っていれば初めの値に戻す
napi_value AiForget(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return boolean(env, false);
    const std::string path = str_arg(env, argv[0]);
    std::lock_guard<std::mutex> lk(g_mu);
    kkc_ai::forget_all(path);   // どのモデルの分も
    if (g_engine) kkc_ai::sync(g_ai, g_engine);
    return boolean(env, true);
}

napi_value Close(napi_env env, napi_callback_info) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_engine) kkc_close(g_engine);
    g_engine = nullptr;
    g_ai = kkc_ai::State();
    unmap(g_lex);
    unmap(g_model);
    return boolean(env, true);
}

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"loadAsync", nullptr, LoadAsync, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"convert", nullptr, Convert, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"convertAsync", nullptr, ConvertAsync, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"complete", nullptr, Complete, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setUserWords", nullptr, SetUserWords, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"times", nullptr, Times, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setSuggest", nullptr, SetSuggest, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setAiLearn", nullptr, SetAiLearn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"aiLearnAsync", nullptr, AiLearnAsync, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"aiForget", nullptr, AiForget, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"close", nullptr, Close, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

napi_module kkc_module = {1, 0, nullptr, Init, "kkc", nullptr, {0}};

}  // namespace

extern "C" __attribute__((constructor)) void RegisterKkcModule() { napi_module_register(&kkc_module); }
