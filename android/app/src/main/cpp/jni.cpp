// Android の IME (com.shunti.japaneseime.Engine) から呼ぶ JNI。辞書とモデルは APK の中の無圧縮の資産を
// そのままメモリに写して使う (AAsset_getBuffer。読み込みの時間とメモリの複製がない)。
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>

#include <string>
#include <vector>

#include "ai_learn.h"
#include "engine.h"
#include "phrase_ends.h"

namespace {
struct Handle {
    AAsset* lex = nullptr;
    AAsset* model = nullptr;
    kkc_engine* e = nullptr;
    kkc_ai::State ai;   // AI学習 (設定、既定オフ)
};
}  // namespace

// model = モデルの資産の名前 (kkc_model.bin・軽量の kkc_model_light.bin)。無ければ kkc_model.bin
extern "C" JNIEXPORT jlong JNICALL Java_com_shunti_japaneseime_Engine_nativeOpen(JNIEnv* env, jclass, jobject am_obj, jint threads,
                                                                              jstring model) {
    AAssetManager* am = AAssetManager_fromJava(env, am_obj);
    auto* h = new Handle();
    h->lex = AAssetManager_open(am, "kkc_lex.bin", AASSET_MODE_BUFFER);
    const char* name = env->GetStringUTFChars(model, nullptr);
    h->model = AAssetManager_open(am, name, AASSET_MODE_BUFFER);
    env->ReleaseStringUTFChars(model, name);
    if (!h->model) h->model = AAssetManager_open(am, "kkc_model.bin", AASSET_MODE_BUFFER);
    if (h->lex && h->model) {
        const void* a = AAsset_getBuffer(h->lex);
        const void* b = AAsset_getBuffer(h->model);
        if (a && b) h->e = kkc_open(a, size_t(AAsset_getLength(h->lex)), b, size_t(AAsset_getLength(h->model)));
    }
    if (!h->e) {
        if (h->lex) AAsset_close(h->lex);
        if (h->model) AAsset_close(h->model);
        delete h;
        return 0;
    }
    kkc_set_threads(h->e, threads);
    return reinterpret_cast<jlong>(h);
}

extern "C" JNIEXPORT void JNICALL Java_com_shunti_japaneseime_Engine_nativeClose(JNIEnv*, jclass, jlong p) {
    auto* h = reinterpret_cast<Handle*>(p);
    if (!h) return;
    kkc_close(h->e);
    AAsset_close(h->lex);
    AAsset_close(h->model);
    delete h;
}

// 候補の配列 (最大 maxout)。times には [網, 下書き, エンコーダ, 区間と語, 上位k] のミリ秒を入れる (null 可)
extern "C" JNIEXPORT jobjectArray JNICALL Java_com_shunti_japaneseime_Engine_nativeConvert(JNIEnv* env, jclass, jlong p, jstring ctx,
                                                                                 jstring kana, jint maxout, jboolean model,
                                                                                 jdoubleArray times) {
    auto* h = reinterpret_cast<Handle*>(p);
    jclass str = env->FindClass("java/lang/String");
    if (!h) return env->NewObjectArray(0, str, nullptr);
    const jchar* c = env->GetStringChars(ctx, nullptr);
    const jchar* k = env->GetStringChars(kana, nullptr);
    std::vector<uint16_t> out(1 << 15);
    int r = kkc_convert(h->e, reinterpret_cast<const uint16_t*>(c), env->GetStringLength(ctx), reinterpret_cast<const uint16_t*>(k),
                        env->GetStringLength(kana), maxout, model ? 1 : 0, out.data(), int(out.size()));
    env->ReleaseStringChars(ctx, c);
    env->ReleaseStringChars(kana, k);
    if (times) {
        double t[5];
        kkc_last_times(h->e, t);
        env->SetDoubleArrayRegion(times, 0, 5, t);
    }
    if (r < 0) r = 0;
    jobjectArray arr = env->NewObjectArray(r, str, nullptr);
    size_t st = 0;
    for (int i = 0; i < r; i++) {
        size_t e = st;
        while (out[e]) e++;
        jstring s = env->NewString(reinterpret_cast<const jchar*>(out.data() + st), jsize(e - st));
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);
        st = e + 1;
    }
    return arr;
}

// 提案 (kkc_set_suggest / kkc_last_suggest)。各要素は「種類 + \u0001 + 表記」
// 次の変換 1 回に、PC と同じくローマ字で打ったキーの並びを渡す (英字の打ち分け・ローマ字の打ち間違いの直し。2026-10-24)
extern "C" JNIEXPORT void JNICALL Java_com_shunti_japaneseime_Engine_nativeSetKeys(JNIEnv* env, jclass, jlong p, jstring keys, jintArray spanEnd) {
    auto* h = reinterpret_cast<Handle*>(p);
    if (!h) return;
    const jchar* k = env->GetStringChars(keys, nullptr);
    jint* sp = env->GetIntArrayElements(spanEnd, nullptr);
    kkc_set_keys(h->e, reinterpret_cast<const uint16_t*>(k), env->GetStringLength(keys), reinterpret_cast<const int32_t*>(sp), env->GetArrayLength(spanEnd));
    env->ReleaseIntArrayElements(spanEnd, sp, JNI_ABORT);
    env->ReleaseStringChars(keys, k);
}

extern "C" JNIEXPORT void JNICALL Java_com_shunti_japaneseime_Engine_nativeSetSuggest(JNIEnv*, jclass, jlong p, jboolean on, jint charFilter) {
    auto* h = reinterpret_cast<Handle*>(p);
    if (!h) return;
    kkc_set_suggest(h->e, on ? 1 : 0);
    kkc_set_char_filter(h->e, charFilter);
}

extern "C" JNIEXPORT jobjectArray JNICALL Java_com_shunti_japaneseime_Engine_nativeLastSuggest(JNIEnv* env, jclass, jlong p) {
    auto* h = reinterpret_cast<Handle*>(p);
    jclass str = env->FindClass("java/lang/String");
    std::vector<uint16_t> buf(8192);
    int32_t kinds[32];
    int n = h ? kkc_last_suggest(h->e, buf.data(), int(buf.size()), kinds, 32) : 0;
    if (n < 0) n = 0;
    jobjectArray arr = env->NewObjectArray(n, str, nullptr);
    size_t st = 0;
    for (int i = 0; i < n; i++) {
        size_t e = st;
        while (buf[e]) e++;
        std::vector<jchar> t;
        t.push_back(jchar(u'0' + kinds[i]));
        t.push_back(jchar(1));
        for (size_t j = st; j < e; j++) t.push_back(jchar(buf[j]));
        jstring s = env->NewString(t.data(), jsize(t.size()));
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);
        st = e + 1;
    }
    return arr;
}

extern "C" JNIEXPORT void JNICALL Java_com_shunti_japaneseime_Engine_nativeSetThreads(JNIEnv*, jclass, jlong p, jint n) {
    auto* h = reinterpret_cast<Handle*>(p);
    if (h) kkc_set_threads(h->e, n);
}

// 直前の変換の 1 位の候補の語の区切り: [終わりの位置0, 表記の長さ0, 終わりの位置1, 表記の長さ1, ...]
// (nativeConvert と同じスレッドで、その直後に呼ぶ)
extern "C" JNIEXPORT jintArray JNICALL Java_com_shunti_japaneseime_Engine_nativeSegments(JNIEnv* env, jclass, jlong p) {
    auto* h = reinterpret_cast<Handle*>(p);
    int32_t ends[512], lens[512];
    int n = h ? kkc_last_segments(h->e, ends, lens, 512) : 0;
    if (n < 0) n = 0;
    std::vector<jint> v;
    for (int i = 0; i < n; i++) { v.push_back(ends[i]); v.push_back(lens[i]); }
    jintArray a = env->NewIntArray(jsize(v.size()));
    if (!v.empty()) env->SetIntArrayRegion(a, 0, jsize(v.size()), v.data());
    return a;
}

// 直前の変換の 1 位を文節 (自立語 + 付属語) にまとめた、文節ごとの読みの終わり (PC 版と同じ決まり。phrase_ends.h)。
// top = nativeConvert が返した 1 位。nativeConvert と同じスレッドで、その直後に呼ぶ
extern "C" JNIEXPORT jintArray JNICALL Java_com_shunti_japaneseime_Engine_nativePhraseEnds(JNIEnv* env, jclass, jlong p, jstring kana, jstring top) {
    auto* h = reinterpret_cast<Handle*>(p);
    int32_t out[512];
    int n = 0;
    if (h) {
        const jsize nk = env->GetStringLength(kana), nt = env->GetStringLength(top);
        const jchar* k = env->GetStringChars(kana, nullptr);
        const jchar* t = env->GetStringChars(top, nullptr);
        n = kkc_phrase_ends(h->e, reinterpret_cast<const uint16_t*>(k), int(nk), reinterpret_cast<const uint16_t*>(t), int(nt), out, 512);
        env->ReleaseStringChars(kana, k);
        env->ReleaseStringChars(top, t);
    }
    jintArray a = env->NewIntArray(jsize(n));
    if (n > 0) env->SetIntArrayRegion(a, 0, jsize(n), reinterpret_cast<const jint*>(out));
    return a;
}

// 直前の変換の上位の経路。1 本ごとに "読みの終わり\1表記\1読みの終わり\1表記…" (nativeConvert と同じスレッドで、その直後に呼ぶ)
extern "C" JNIEXPORT jobjectArray JNICALL Java_com_shunti_japaneseime_Engine_nativePaths(JNIEnv* env, jclass, jlong p) {
    auto* h = reinterpret_cast<Handle*>(p);
    constexpr int PM = 30, WC = 1024, TC = 4096;
    std::vector<double> costs(PM);
    std::vector<int32_t> cnt(PM), pe(WC), pl(WC);
    std::vector<uint16_t> tx(TC);
    const int np = h ? kkc_last_paths(h->e, PM, costs.data(), cnt.data(), pe.data(), pl.data(), WC, tx.data(), TC) : 0;
    jobjectArray out = env->NewObjectArray(np, env->FindClass("java/lang/String"), nullptr);
    size_t wi = 0, ti = 0;
    for (int i = 0; i < np; i++) {
        std::u16string s;
        for (int k = 0; k < cnt[size_t(i)]; k++, wi++) {
            if (k) s += u'\1';
            for (char c : std::to_string(pe[wi])) s += char16_t(c);
            s += u'\1';
            s.append(reinterpret_cast<const char16_t*>(tx.data() + ti), size_t(pl[wi]));
            ti += size_t(pl[wi]);
        }
        jstring js = env->NewString(reinterpret_cast<const jchar*>(s.data()), jsize(s.size()));
        env->SetObjectArrayElement(out, i, js);
        env->DeleteLocalRef(js);
    }
    return out;
}

// 直前の変換の 1 位の語ごとの品詞の左 ID (nativeSegments と同じ並び。PC 版の Conversion の words の lid)。nativeConvert の直後に同じスレッドで
extern "C" JNIEXPORT jintArray JNICALL Java_com_shunti_japaneseime_Engine_nativeSegmentPos(JNIEnv* env, jclass, jlong p) {
    auto* h = reinterpret_cast<Handle*>(p);
    int32_t lids[512], rids[512];
    int n = h ? kkc_last_segment_pos(h->e, lids, rids, 512) : 0;
    if (n < 0) n = 0;
    jintArray a = env->NewIntArray(jsize(n));
    if (n > 0) env->SetIntArrayRegion(a, 0, jsize(n), reinterpret_cast<const jint*>(lids));
    return a;
}

// 確定した文 (読み, 表記) になる経路のうちコストのいちばん低いものの語ごとの [読みの終わり, 表記の長さ, 左 ID, 右 ID] を並べたもの
// (PC 版 Converter::path_pos。品詞ごとの学習で、選んだ文の中での品詞を決める。2026-11-09)。経路が無ければ空。nativeConvert と同じスレッドで
extern "C" JNIEXPORT jintArray JNICALL Java_com_shunti_japaneseime_Engine_nativePathPos(JNIEnv* env, jclass, jlong p, jstring reading, jstring text) {
    auto* h = reinterpret_cast<Handle*>(p);
    std::vector<jint> v;
    if (h) {
        const jsize nr = env->GetStringLength(reading), nt = env->GetStringLength(text);
        const size_t cap = size_t(nr) + 1;
        std::vector<int32_t> ends(cap), l16(cap), lids(cap), rids(cap);
        const jchar* r = env->GetStringChars(reading, nullptr);
        const jchar* t = env->GetStringChars(text, nullptr);
        const int n = kkc_path_pos(h->e, reinterpret_cast<const uint16_t*>(r), int(nr), reinterpret_cast<const uint16_t*>(t), int(nt),
                                   ends.data(), l16.data(), lids.data(), rids.data(), int(cap));
        env->ReleaseStringChars(reading, r);
        env->ReleaseStringChars(text, t);
        for (int i = 0; i < n; i++) { v.push_back(ends[size_t(i)]); v.push_back(l16[size_t(i)]); v.push_back(lids[size_t(i)]); v.push_back(rids[size_t(i)]); }
    }
    jintArray a = env->NewIntArray(jsize(v.size()));
    if (!v.empty()) env->SetIntArrayRegion(a, 0, jsize(v.size()), v.data());
    return a;
}

// 品詞の表 (desktop/core/pos_classes.h の POS_CLASSES・POS_GROUPS、左 ID ごとに 1 字)。[分類, 仲間]
extern "C" JNIEXPORT jobjectArray JNICALL Java_com_shunti_japaneseime_Engine_nativePosTables(JNIEnv* env, jclass) {
    jobjectArray out = env->NewObjectArray(2, env->FindClass("java/lang/String"), nullptr);
    std::string c(shunti::POS_CLASSES, size_t(shunti::POS_CLASS_COUNT)), g(shunti::POS_GROUPS, size_t(shunti::POS_CLASS_COUNT));
    jstring a = env->NewStringUTF(c.c_str()), b = env->NewStringUTF(g.c_str());
    env->SetObjectArrayElement(out, 0, a);
    env->SetObjectArrayElement(out, 1, b);
    return out;
}

// 学習した語を、選んだときの品詞の仲間の中でいちばん安い辞書の項目を見本にして網に置く (PC 版 set_user_words と同じ
// kkc_user_add_pos + POS_GROUPS。2026-11-09)。辞書にその仲間の項目が無ければ 0。nativeConvert と同じスレッドで
extern "C" JNIEXPORT jint JNICALL Java_com_shunti_japaneseime_Engine_nativeUserAddPos(JNIEnv* env, jclass, jlong p, jstring r, jstring s,
                                                                                     jint bonus, jint flags, jint lid) {
    auto* h = reinterpret_cast<Handle*>(p);
    if (!h || lid < 0) return 0;
    const jsize nr = env->GetStringLength(r), ns = env->GetStringLength(s);
    const jchar* a = env->GetStringChars(r, nullptr);
    const jchar* b = env->GetStringChars(s, nullptr);
    const int ok = kkc_user_add_pos(h->e, reinterpret_cast<const uint16_t*>(a), int(nr), reinterpret_cast<const uint16_t*>(b), int(ns), bonus, flags, lid,
                                    shunti::POS_GROUPS, shunti::POS_CLASS_COUNT);
    env->ReleaseStringChars(r, a);
    env->ReleaseStringChars(s, b);
    return ok;
}

// ユーザー辞書 (kkc_user_clear / kkc_user_add_like)。nativeConvert と同じスレッドで呼ぶ
extern "C" JNIEXPORT void JNICALL Java_com_shunti_japaneseime_Engine_nativeUserClear(JNIEnv*, jclass, jlong p) {
    auto* h = reinterpret_cast<Handle*>(p);
    if (h) kkc_user_clear(h->e);
}

extern "C" JNIEXPORT jint JNICALL Java_com_shunti_japaneseime_Engine_nativeUserAddLike(JNIEnv* env, jclass, jlong p, jstring r, jstring s,
                                                                                     jstring tr, jstring ts, jint bonus) {
    auto* h = reinterpret_cast<Handle*>(p);
    if (!h) return 0;
    jstring strs[4] = {r, s, tr, ts};
    const jchar* c[4];
    jsize n[4];
    for (int i = 0; i < 4; i++) {
        c[i] = env->GetStringChars(strs[i], nullptr);
        n[i] = env->GetStringLength(strs[i]);
    }
    auto u = [&](int i) { return reinterpret_cast<const uint16_t*>(c[i]); };
    int ok = kkc_user_add_like(h->e, u(0), n[0], u(1), n[1], u(2), n[2], u(3), n[3], bonus);
    for (int i = 0; i < 4; i++) env->ReleaseStringChars(strs[i], c[i]);
    return ok;
}

// 予測 (kkc_complete): 読みが prefix で始まる、より長い辞書の語 (よく使う順)。辞書を読むだけなので、どのスレッドから呼んでもよい
extern "C" JNIEXPORT jobjectArray JNICALL Java_com_shunti_japaneseime_Engine_nativeComplete(JNIEnv* env, jclass, jlong p, jstring prefix,
                                                                                          jint maxExtra, jint maxOut) {
    auto* h = reinterpret_cast<Handle*>(p);
    jclass str = env->FindClass("java/lang/String");
    if (!h) return env->NewObjectArray(0, str, nullptr);
    const jchar* c = env->GetStringChars(prefix, nullptr);
    std::vector<uint16_t> out(1 << 12);
    int n = kkc_complete(h->e, reinterpret_cast<const uint16_t*>(c), env->GetStringLength(prefix), maxExtra, maxOut, out.data(), int(out.size()));
    env->ReleaseStringChars(prefix, c);
    if (n < 0) n = 0;
    jobjectArray arr = env->NewObjectArray(n, str, nullptr);
    size_t st = 0;
    for (int i = 0; i < n; i++) {
        size_t e = st;
        while (out[e]) e++;
        jstring s = env->NewString(reinterpret_cast<const jchar*>(out.data() + st), jsize(e - st));
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);
        st = e + 1;
    }
    return arr;
}

namespace {
std::u16string jstr16(JNIEnv* env, jstring s) {
    const jchar* c = env->GetStringChars(s, nullptr);
    std::u16string out(reinterpret_cast<const char16_t*>(c), size_t(env->GetStringLength(s)));
    env->ReleaseStringChars(s, c);
    return out;
}
}  // namespace

// AI学習のオン・オフ。path = 育てた中身の保存先 (空ならオフ)。同じ保存先なら、消された (学習のリセット) かだけを見る
extern "C" JNIEXPORT void JNICALL Java_com_shunti_japaneseime_Engine_nativeAiSet(JNIEnv* env, jclass, jlong p, jstring path) {
    auto* h = reinterpret_cast<Handle*>(p);
    if (!h) return;
    const char* c = env->GetStringUTFChars(path, nullptr);
    const std::string s(c);
    env->ReleaseStringUTFChars(path, c);
    kkc_ai::set(h->ai, h->e, s);
}

// 確定した文から 1 歩学ぶ。返り値 = 学ぶ前の損失 (学ばなかったら負)
extern "C" JNIEXPORT jfloat JNICALL Java_com_shunti_japaneseime_Engine_nativeAiLearn(JNIEnv* env, jclass, jlong p, jstring ctx, jstring reading,
                                                                                jstring text) {
    auto* h = reinterpret_cast<Handle*>(p);
    if (!h) return -1;
    return kkc_ai::learn(h->ai, h->e, jstr16(env, ctx), jstr16(env, reading), jstr16(env, text));
}
