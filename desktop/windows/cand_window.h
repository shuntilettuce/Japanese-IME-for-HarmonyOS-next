// 候補の窓 (変換の候補を 9 個ずつ番号つきで出す。GDI で描く小さな窓。入力欄のアプリのプロセスの中で動く)
#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <functional>
#include <string>
#include <vector>

#include "../core/converter.h"
#include "../core/text.h"

namespace shunti {
namespace win {

class CandWindow {
public:
    ~CandWindow();
    // anchor = 選んでいる文節の字の画面上の四角。owner = 入力欄の窓
    // sugg = 提案 (候補の上の 1 行。空なら出さない)
    // sugg = 提案 (いちばん下の行の左に札で並べる)。sugg_sel = 選んでいる提案 (-1 = 提案の列に入っていない)
    void show(const std::vector<u16>& cands, int sel, const RECT& anchor, HWND owner, const std::vector<u16>& sugg = {},
              int sugg_sel = -1, const std::vector<u16>& notes = {});   // notes = 候補の右に薄く添える種類
    // 候補の木 (候補の上の帯)。rs〜re = 選んでいる文節の読みの区間。空なら帯を出さない。show の前に呼ぶ
    // short_n = 先頭の木の候補の数 (選ぶ位置がこれ未満のあいだは、これだけを出す。0 = 全部)
    void set_tree(const std::vector<TreeNode>& tree, int rs, int re, int short_n = 0, const std::vector<float>& scores = {}, int fork = -1) {
        tree_ = tree; tree_rs_ = rs; tree_re_ = re; short_ = short_n; scores_ = scores; fork_ = fork;
    }
    void hide();
    void destroy();
    bool visible() const { return (hwnd_ && IsWindowVisible(hwnd_)) || (thwnd_ && IsWindowVisible(thwnd_)); }
    std::function<void(int)> on_click;   // 候補をマウスで選んだ (候補の番号)
    std::function<void(int)> on_sugg_click;   // 提案をマウスで選んだ (提案の番号)
    std::wstring label;                        // 下の行の頭の字 (空なら「提案 Shift+↓」。文の流れの続きでは「続き Tab」)
    std::string theme = "auto";          // auto (Windows に合わせる)・light・dark

private:
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l);
    void paint(HDC dc);
    void layout();
    void layout_tree(HDC dc);
    void paint_tree(Gdiplus::Graphics& g, HDC dc);
    void present_tree(int x, int y);
    RECT pill_rect(HDC dc, const u16& text, float s, int cx, int top) const;   // 候補の木を背景の無い窓 (透明度つき) で出す
    int hit(int x, int y) const;
    int hit_sugg(int x, int y) const;
    std::wstring sugg_label() const;
    std::wstring foot_text(int n) const;

    HWND hwnd_ = nullptr;
    HWND thwnd_ = nullptr;   // 候補の木の窓 (背景無し)
    HWND owner_ = nullptr;
    HFONT font_ = nullptr, small_ = nullptr;
    UINT dpi_ = 0;
    std::vector<u16> cands_;
    std::vector<u16> notes_;
    std::vector<u16> sugg_;
    std::vector<std::pair<int, int>> sugg_x_;   // 提案の札の左右 (当たり判定)
    int sugg_sel_ = -1;
    int sel_ = 0, page_ = 0;
    // 候補の木の帯の描く物 (layout_tree が作り、paint_tree が描く)
    struct TChip { RECT r; u16 text; float s; bool sel, ghost; };
    struct TText { int x, y; u16 text; bool tiny; float s; };   // 点数・別の候補 (x は中央)
    struct TLine { float x1, y1, x2, y2, s; };
    struct TPath { std::vector<std::pair<float, float>> pts; float s; bool gray = false; int owner = -1; };   // owner = 別の候補の札の中央 x (かぶって消すとき)   // 二股の配線 (折れ線)
    std::vector<TreeNode> tree_;
    int tree_rs_ = -1, tree_re_ = -1, short_ = 0;
    std::vector<float> scores_;   // 候補ごとの木の強さ (-1 = 分からない)
    int fork_ = -1;                // 別の区切りの候補の番号
    int colx_ = 0, colw_ = 0;      // 候補の列の左と札の幅
    std::vector<int> tseps_;       // 候補の列の区切りの線の y
    std::vector<TChip> tchips_;
    std::vector<TChip> tcands_;      // 選んでいる文節の候補の列 (s = 木の強さ、無ければ -1)
    std::vector<int> tcand_idx_;     // tcands_ の候補の番号
    std::vector<TText> ttexts_;
    std::vector<TLine> tlines_;
    std::vector<TPath> tpaths_;
    std::vector<std::pair<float, float>> tdots_;   // 二股の印の位置
    int tree_w_ = 0, tree_h_ = 0, list_x_ = 0, lw_ = 0, lh_ = 0;   // lw_・lh_ = 候補の一覧の幅と高さ      // 帯の幅と高さ、候補の一覧の左 (選んでいる文節の下)
    HFONT tfont_ = nullptr, tsmall_ = nullptr, talt_ = nullptr;
    int row_h_ = 0, pad_ = 0, num_w_ = 0, width_ = 0, height_ = 0, footer_h_ = 0;
};

}  // namespace win
}  // namespace shunti
