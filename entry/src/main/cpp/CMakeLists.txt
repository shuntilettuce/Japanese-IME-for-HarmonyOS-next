# shuntelligence (AI 変換) の推論エンジン libkkc.so。engine.cpp = 網 + Transformer + 上位 k、napi_kkc.cpp = ArkTS との窓口
cmake_minimum_required(VERSION 3.5.0)
project(kkc)
set(CMAKE_CXX_STANDARD 17)
add_library(kkc SHARED napi_kkc.cpp engine.cpp glide.cpp glide_nn.cpp)
target_include_directories(kkc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# 開発用ビルドでも最適化する (-O0 だと行列計算が数倍遅く、打鍵ごとの変換が重くなる)。
# target の指定は CMAKE_CXX_FLAGS_DEBUG の -O0 より後ろに付くので、こちらが勝つ
target_compile_options(kkc PRIVATE -O3 -ffp-contract=fast -fno-math-errno)
target_link_libraries(kkc PUBLIC libace_napi.z.so librawfile.z.so)
