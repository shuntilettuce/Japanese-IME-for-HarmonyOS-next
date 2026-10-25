import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// shunti IME の Android 版 (AI 変換だけ)。外部ライブラリなし。
// - 変換エンジンは HarmonyOS 版と同じ C++ (../entry/src/main/cpp/engine.cpp) を、NDK の clang で直接ビルドする (buildKkc)
// - 辞書とモデルも HarmonyOS 版と同じもの (../entry/src/main/resources/rawfile/kkc_*.bin。python tools/fetch_ai_assets.py で取得)
//   を APK に無圧縮で同梱する (copyAiAssets)。端末ではそのままメモリに写して使うので、インターネット権限は要らない
// - キー配置・記号・絵文字などの表は HarmonyOS 版のソースから生成したもの (node tools/gen_android_tables.mjs)
val repo = rootDir.parentFile
val engineDir = File(repo, "entry/src/main/cpp")
val rawfile = File(repo, "entry/src/main/resources/rawfile")
val ndkVer = "27.2.12479018"

android {
    namespace = "com.shunti.japaneseime"
    compileSdk = 36
    ndkVersion = ndkVer

    defaultConfig {
        // AppGallery では HarmonyOS 版 (bundleName com.shunti.japaneseime) と同じ名前を使えないので .android を足す。
        // コードの中の名前 (namespace・Kotlin のパッケージ・JNI の関数名) は com.shunti.japaneseime のまま
        applicationId = "com.shunti.japaneseime.android"
        minSdk = 26
        targetSdk = 36
        versionCode = 7
        versionName = "1.3.2"
        ndk { abiFilters += listOf("arm64-v8a") }
    }
    // リリースの署名: android/keystore.properties (git には入れない) があれば使う
    val ks = rootProject.file("keystore.properties")
    if (ks.exists()) {
        val p = Properties().apply { ks.inputStream().use { load(it) } }
        signingConfigs {
            create("release") {
                storeFile = rootProject.file(p.getProperty("storeFile"))
                storePassword = p.getProperty("storePassword")
                keyAlias = p.getProperty("keyAlias")
                keyPassword = p.getProperty("keyPassword")
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            if (ks.exists()) signingConfig = signingConfigs.getByName("release")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    // 辞書とモデルは圧縮しない (端末でそのままメモリに写して使う)
    androidResources {
        noCompress += listOf("bin")
    }
    packaging {
        jniLibs { useLegacyPackaging = false }
    }
    sourceSets["main"].jniLibs.srcDir(layout.buildDirectory.dir("kkc/jniLibs"))
    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("kkc/assets"))
}

// libkkc.so (engine.cpp + jni.cpp) を NDK の clang で作る。CMake は使わない (入っていなくてよい)
val buildKkc by tasks.registering(Exec::class) {
    val out = layout.buildDirectory.file("kkc/jniLibs/arm64-v8a/libkkc.so")
    val sources = listOf(File(engineDir, "engine.cpp"), file("src/main/cpp/jni.cpp"))
    inputs.files(sources, File(engineDir, "engine.h"))
    outputs.file(out)
    val os = System.getProperty("os.name").lowercase()
    val host = when {
        os.contains("win") -> "windows-x86_64"
        os.contains("mac") -> "darwin-x86_64"
        else -> "linux-x86_64"
    }
    val exe = if (os.contains("win")) "clang++.exe" else "clang++"
    doFirst { out.get().asFile.parentFile.mkdirs() }
    executable = File(android.sdkDirectory, "ndk/$ndkVer/toolchains/llvm/prebuilt/$host/bin/$exe").path
    args(
        "--target=aarch64-linux-android26", "-O3", "-std=c++17", "-fPIC", "-ffp-contract=fast", "-fno-math-errno",
        "-fvisibility=hidden", "-DNDEBUG", "-I", engineDir.path, "-shared", "-static-libstdc++",
        *sources.map { it.path }.toTypedArray(),
        "-landroid", "-llog", "-Wl,--gc-sections", "-s", "-o", out.get().asFile.path,
    )
}

// 辞書とモデルを HarmonyOS 版の rawfile から取り込む (変わったときだけ写す)
val copyAiAssets by tasks.registering(Copy::class) {
    val names = listOf("kkc_lex.bin", "kkc_model.bin")
    doFirst {
        val missing = names.filter { !File(rawfile, it).exists() }
        if (missing.isNotEmpty()) throw GradleException("辞書とモデルがありません ($missing)。リポジトリの直下で python tools/fetch_ai_assets.py を実行してください")
    }
    from(rawfile) { include(names) }
    // 軽量の変換モデル (設定で選ぶ。リポジトリの models/、tools/fetch_ai_assets.py で取得。無ければ標準だけ)
    from(File(repo, "models")) { include("kkc_model_light.bin") }
    into(layout.buildDirectory.dir("kkc/assets"))
}

tasks.named("preBuild") { dependsOn(buildKkc, copyAiAssets) }

dependencies {
    // テストだけ (APK には入らない)。GoldenTest で HarmonyOS 版と答え合わせをする
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
}
