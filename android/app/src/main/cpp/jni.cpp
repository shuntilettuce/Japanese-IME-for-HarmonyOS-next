// Android の IME (com.shunti.japaneseime.Engine) から呼ぶ JNI。辞書とモデルは APK の中の無圧縮の資産を
// そのままメモリに写して使う (AAsset_getBuffer。読み込みの時間とメモリの複製がない)。
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>

#include <vector>

#include "engine.h"

namespace {
struct Handle {
    AAsset* lex = nullptr;
    AAsset* model = nullptr;
    kkc_engine* e = nullptr;
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
// 次の変換 1 回に、PC と同じくローマ字で打った鍵の列を渡す (英字の打ち分け・ローマ字の打ち間違いの直し。2026-10-24)
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
