# Linux 版 (Fcitx5 の入力メソッドのアドオン)。変換は Windows 版と同じ共通部分 (desktop/core) と変換エンジン (entry/src/main/cpp/engine.cpp)。
#   cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && sudo cmake --install build
# 辞書とモデル (entry/src/main/resources/rawfile/kkc_*.bin) は share/shunti-ime に入る (python tools/fetch_ai_assets.py で取得)。
cmake_minimum_required(VERSION 3.16)
project(fcitx5-shunti VERSION 0.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)   # Fcitx5 5.1 の見出し (std::span など) が C++20 を要る
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Fcitx5Core REQUIRED)
find_package(Threads REQUIRED)
include(GNUInstallDirs)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(CORE ${ROOT}/desktop/core)

add_library(shunti MODULE
  src/shunti.cpp
  ${CORE}/text.cpp ${CORE}/romaji.cpp ${CORE}/store.cpp ${CORE}/settings.cpp ${CORE}/converter.cpp ${CORE}/composer.cpp ${CORE}/flow.cpp ${CORE}/datetime.cpp
  ${ROOT}/entry/src/main/cpp/engine.cpp)
target_include_directories(shunti PRIVATE ${ROOT}/entry/src/main/cpp)
target_link_libraries(shunti PRIVATE Fcitx5::Core Threads::Threads)
set_target_properties(shunti PROPERTIES PREFIX "")

configure_file(shunti-addon.conf.in ${CMAKE_CURRENT_BINARY_DIR}/shunti-addon.conf @ONLY)
install(TARGETS shunti DESTINATION "${FCITX_INSTALL_ADDONDIR}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/shunti-addon.conf RENAME shunti.conf DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon")
install(FILES shunti.conf DESTINATION "${FCITX_INSTALL_PKGDATADIR}/inputmethod")
install(FILES icons/shunti-ime.png DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/64x64/apps")
# ライセンスの表記は Windows 版と同じもの (見出しだけ Linux 版に)
file(READ ${ROOT}/desktop/windows/LICENSE.txt LICENSE_TEXT)
string(REPLACE "(Windows 版)" "(Linux 版)" LICENSE_TEXT "${LICENSE_TEXT}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/copyright "${LICENSE_TEXT}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/copyright README.md DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/fcitx5-shunti")
foreach(f kkc_lex.bin kkc_model.bin)
  if(EXISTS ${ROOT}/entry/src/main/resources/rawfile/${f})
    install(FILES ${ROOT}/entry/src/main/resources/rawfile/${f} DESTINATION "${CMAKE_INSTALL_DATADIR}/shunti-ime")
  else()
    message(WARNING "${f} がありません (python tools/fetch_ai_assets.py で取得してください)。変換はできません")
  endif()
endforeach()
# 軽量・高精度の変換モデル (設定で選ぶ。無ければ標準だけ)
foreach(f kkc_model_light.bin)
  if(EXISTS ${ROOT}/models/${f})
    install(FILES ${ROOT}/models/${f} DESTINATION "${CMAKE_INSTALL_DATADIR}/shunti-ime")
  endif()
endforeach()

# .deb (Debian・Ubuntu 向け): cpack -G DEB
set(CPACK_PACKAGE_NAME "fcitx5-shunti")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "shuntilettuce")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "shunti IME: AI kana-kanji conversion (shuntelligence) for Fcitx5")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "fcitx5")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
