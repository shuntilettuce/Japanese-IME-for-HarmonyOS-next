#include <cstdlib>
#include "converter.h"

#include <cmath>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

#include "engine.h"
#include "pos_classes.h"
#include "tables.h"

namespace shunti {

namespace {
const uint16_t* p16(const u16& s) { return reinterpret_cast<const uint16_t*>(s.data()); }

char pos_class(int id) { return id >= 0 && id < POS_CLASS_COUNT ? POS_CLASSES[id] : '0'; }

bool is_punct(const u16& s) {
    return !s.empty() && s.find_first_not_of(u"、。，．！？!?…‥・,.") == u16::npos;
}

// 長すぎる読みを分ける位置 (真ん中より前の最後の句読点の後ろ。無ければ真ん中)
size_t split_point(const u16& kana) {
    size_t mid = std::min<size_t>(kana.size() / 2, 120);
    for (size_t i = mid; i > mid / 2; i--)
        if (u16(u"、。！？!?").find(kana[i - 1]) != u16::npos) return i;
    return mid;
}
}  // namespace

// 不適切な語も候補に出す (ユーザー 2026-10-13「不適切語は常時出していい」。スマホ版の設定の既定と同じ)
bool allow_slurs = true;

bool is_slur(const u16& s) {
    for (const auto& r : SLURS) {
        u16 w = r.word;
        for (size_t i = s.find(w); i != u16::npos; i = s.find(w, i + 1)) {
            bool excused = false;
            for (int k = 0; r.not_after[k]; k++)
                if (s.compare(i + w.size(), u16(r.not_after[k]).size(), r.not_after[k]) == 0) excused = true;
            if (!excused) return true;
        }
    }
    return false;
}

Converter::~Converter() {
    ai_stop_bulk();
    if (e_) kkc_close(e_);
}

bool Converter::open(const void* lex, size_t lex_size, const void* model, size_t model_size, int threads) {
    kkc_engine* next = kkc_open(lex, lex_size, model, model_size);
    if (!next) return false;   // 開けなければ今のエンジンのまま
    ai_stop_bulk();   // 裏で学んでいるのは前のモデル (呼び出し側はこの後に外す)
    lex_ = lex;
    lex_size_ = lex_size;
    model_ = model;
    model_size_ = model_size;
    if (e_) kkc_close(e_);
    e_ = next;
    if (e_ && threads > 0) kkc_set_threads(e_, threads);
    if (e_) kkc_set_suggest(e_, suggest_ ? 1 : 0);
    if (e_) kkc_set_latin(e_, latin_ ? 1 : 0);
    if (e_) kkc_set_latin_mid2(e_, mid2_ ? 1 : 0);
    if (e_ && (!typo_ || typo_bias_ != 0.f)) kkc_set_typo(e_, typo_ ? 1 : 0, typo_bias_);
    if (e_ && !variants_.empty()) set_variants(variants_);
    if (e_ && !user_.empty()) set_user_words(std::vector<UserDict::Form>(user_), user_bonus_);   // モデルを替えて開き直したとき
    ai_ready_ = false;
    if (e_ && !ai_base_.empty()) set_ai_learn(std::filesystem::path(ai_base_));   // 新しいモデルの保存先に替わる
    return e_ != nullptr;
}

// ---------------------------------------------------------------- AI学習
// 保存の形: "SHA2" + 学んだ記録の時刻 (int64、確定した文の記録 = recent_learn.tsv の C 行のうち、この時刻までは学んだ)
// + 学んだときの記録の大きさ (int64、変わっていなければ記録を読み直さない) + 個数 (int32) + float の並び。
// モデルを替えて個数が合わなければ初めから育て直す
namespace {
// 学ぶ速さ (v・W・網・初めの値へ引き戻す強さ・上乗せの上限)。確定の記録で 1 位が最もよく、文の確率の損失も下がった組
void ai_config(kkc_engine* e) { kkc_romance_config(e, 0.2f, 0.01f, 0.05f, 0.01f, 2.0f); }

long long unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

long long log_bytes(const std::filesystem::path& dir) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(dir / RECENT_FILE, ec);
    return ec ? 0 : static_cast<long long>(n);
}

bool ai_read(const std::filesystem::path& path, kkc_engine* e, long long& until, long long& bytes) {
    std::ifstream f(path, std::ios::binary);
    char magic[4] = {};
    int64_t u = 0, b = 0;
    int32_t n = 0;
    if (!f.read(magic, 4) || std::string(magic, 4) != "SHA2" || !f.read(reinterpret_cast<char*>(&u), 8) ||
        !f.read(reinterpret_cast<char*>(&b), 8) || !f.read(reinterpret_cast<char*>(&n), 4) || n != kkc_romance_size(e))
        return false;
    std::vector<float> v(static_cast<size_t>(n));
    if (!f.read(reinterpret_cast<char*>(v.data()), std::streamsize(v.size() * 4))) return false;
    kkc_romance_set(e, v.data(), n);
    until = u;
    bytes = b;
    return true;
}

bool ai_write(const std::filesystem::path& path, kkc_engine* e, long long until, long long bytes) {
    const int n = kkc_romance_size(e);
    if (n <= 0) return false;
    std::vector<float> v(static_cast<size_t>(n));
    if (kkc_romance_get(e, v.data(), n) != n) return false;
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    bool ok;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        const int64_t u = until, b = bytes;
        const int32_t n32 = n;
        f.write("SHA2", 4);
        f.write(reinterpret_cast<const char*>(&u), 8);
        f.write(reinterpret_cast<const char*>(&b), 8);
        f.write(reinterpret_cast<const char*>(&n32), 4);
        f.write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size() * 4));
        ok = bool(f);
    }
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);
    return ok && !ec;
}

// 確定した読みをモデルで変換し直して網を作り、その網で確定した表記の経路の確率を上げる。返り値 = 学ぶ前の損失 (学ばなかったら負)
float ai_step(kkc_engine* e, const u16& ctx, const u16& reading, const u16& text) {
    if (reading.empty() || text.empty() || text == reading) return -1;
    std::vector<uint16_t> buf(size_t(reading.size()) * 40 + 256);
    const int n = kkc_convert(e, p16(ctx), int(ctx.size()), p16(reading), int(reading.size()), 10, 1, buf.data(), int(buf.size()));
    if (n <= 0) return -1;   // 長すぎる読みなどは学ばない
    float loss = 0;
    return kkc_romance_learn(e, p16(text), int(text.size()), &loss) == 1 ? loss : -1;
}

// 確定した文の記録のうち、時刻が since より後の分を古い順に学ぶ。返り値 = 読んだ最後の時刻 (無ければ since)
long long ai_learn_log(kkc_engine* e, const std::filesystem::path& dir, long long since, const std::atomic<bool>& stop, int& learned) {
    std::string s;
    if (!read_file(dir / RECENT_FILE, s)) return since;
    long long last = since;
    for (size_t st = 0; st < s.size() && !stop;) {
        size_t en = s.find('\n', st);
        if (en == std::string::npos) en = s.size();
        const std::string line = s.substr(st, en - st);
        st = en + 1;
        // 時刻 \t C \t 読み \t 表記 \t 左の文脈
        std::vector<std::string> f;
        for (size_t a = 0;;) {
            const size_t t = line.find('\t', a);
            f.push_back(line.substr(a, t == std::string::npos ? t : t - a));
            if (t == std::string::npos) break;
            a = t + 1;
        }
        if (f.size() != 5 || f[1] != "C") continue;
        long long t = 0;
        for (char c : f[0]) if (c >= '0' && c <= '9') t = t * 10 + (c - '0');
        if (t <= since) continue;
        if (ai_step(e, from_utf8(f[4]), from_utf8(f[2]), from_utf8(f[3])) >= 0) learned++;
        last = std::max(last, t);
    }
    return last;
}

std::FILE* open_new(const std::filesystem::path& p) {   // 無いときだけ作る (ほかの窓と取り合わない)
#ifdef _WIN32
    return _wfopen(p.c_str(), L"wx");
#else
    return std::fopen(p.c_str(), "wx");
#endif
}
}  // namespace

// 保存先はモデルごと (…/ai_learn.bin → …/ai_learn_<モデルの印>.bin)。AI学習の中身はモデルの内側の数に掛ける重みで、
// ほかのモデルには使えない。分けておけば、軽量・標準を切り替えても消えない (2026-11-06)
static std::filesystem::path ai_model_path(const std::filesystem::path& base, kkc_engine* e) {
    if (base.empty() || !e) return base;
    char tag[16];
    std::snprintf(tag, sizeof tag, "_%08x", unsigned(kkc_model_tag(e)));
    return base.parent_path() / (base.stem().string() + tag + ".bin");
}

namespace {
// 前の 1 つだけの保存 (ai_learn.bin) を、個数が合えば今のモデルの名前に移す。使っていないモデルの分は新しい 4 つまで
void ai_adopt_and_prune(const std::filesystem::path& base, const std::filesystem::path& path, kkc_engine* e) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && std::filesystem::exists(base, ec)) {
        long long u = 0, b = 0;
        std::vector<float> keep(size_t(std::max(kkc_romance_size(e), 0)));
        if (!keep.empty()) kkc_romance_get(e, keep.data(), int(keep.size()));
        if (ai_read(base, e, u, b)) std::filesystem::rename(base, path, ec);
        if (!keep.empty()) kkc_romance_set(e, keep.data(), int(keep.size()));
    }
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> others;
    for (auto it = std::filesystem::directory_iterator(path.parent_path(), ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (it->path() == path || n.rfind("ai_learn_", 0) != 0 || it->path().extension() != ".bin") continue;
        std::error_code tc;
        others.push_back({std::filesystem::last_write_time(it->path(), tc), it->path()});
    }
    std::sort(others.begin(), others.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t k = 4; k < others.size(); k++) std::filesystem::remove(others[k].second, ec);
}
}  // namespace

void Converter::set_ai_learn(const std::filesystem::path& base) {
    if (ai_ready_ && base == ai_base_) return;
    const std::filesystem::path path = ai_model_path(base, e_);
    ai_stop_bulk();
    ai_ready_ = e_ != nullptr;
    ai_base_ = base;
    ai_path_ = path;
    ai_seen_ = {};
    ai_until_ = 0;
    ai_bytes_ = 0;
    if (!e_) return;
    kkc_romance_enable(e_, path.empty() ? 0 : 1);
    if (path.empty()) return;
    kkc_romance_reset(e_);
    ai_config(e_);
    ai_adopt_and_prune(base, path, e_);
    ai_sync();
    ai_start_bulk();
}

void Converter::ai_sync() {
    if (ai_busy_) return;   // 記録から学んでいる間は、終わって保存したものを後で読む
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(ai_path_, ec);
    if (ec) {   // 無い: 消された (学習の全消去) なら初めの値に戻す
        if (ai_seen_ != std::filesystem::file_time_type{}) kkc_romance_reset(e_);
        ai_seen_ = {};
        ai_until_ = ai_bytes_ = 0;
        return;
    }
    if (t == ai_seen_) return;
    ai_seen_ = t;
    if (!ai_read(ai_path_, e_, ai_until_, ai_bytes_)) { kkc_romance_reset(e_); ai_until_ = ai_bytes_ = 0; }
}

void Converter::ai_save() {
    ai_until_ = unix_now();   // 確定した文は、学ぶ前に記録に書いてある
    ai_bytes_ = log_bytes(ai_path_.parent_path());
    std::error_code ec;
    if (ai_write(ai_path_, e_, ai_until_, ai_bytes_)) ai_seen_ = std::filesystem::last_write_time(ai_path_, ec);
}

// オンにしたとき、まだ学んでいない確定した文の記録 (設定の「学習を始める」で残した分) を裏でまとめて学ぶ。
// 変換と取り合わないよう、別に開いたエンジンで学び、終わったら保存する (変換の側は次に使うときに読み直す)
void Converter::ai_start_bulk() {
    const std::filesystem::path dir = ai_path_.parent_path();
    const long long bytes = log_bytes(dir);
    if (bytes == 0 || bytes == ai_bytes_ || !lex_ || !model_) return;   // 記録が無い・前に学んだときから変わっていない
    // ほかの窓 (別のプロセス) が学んでいる最中なら任せる。30 分より古い印は、途中で落ちた残りとして取り直す
    const std::filesystem::path lock = dir / L"ai_learn.lock";
    std::error_code ec;
    const auto lt = std::filesystem::last_write_time(lock, ec);
    if (!ec && std::filesystem::file_time_type::clock::now() - lt < std::chrono::minutes(30)) return;
    if (!ec) std::filesystem::remove(lock, ec);
    std::FILE* lf = open_new(lock);
    if (!lf) return;
    std::fclose(lf);
    const int n = kkc_romance_size(e_);
    std::vector<float> init(static_cast<size_t>(std::max(n, 0)));
    if (n > 0) kkc_romance_get(e_, init.data(), n);
    ai_stop_ = false;
    ai_busy_ = true;
    ai_bulk_ = std::thread([this, path = ai_path_, dir, lock, init, until = ai_until_] {
        kkc_engine* b = kkc_open(lex_, lex_size_, model_, model_size_);
        if (b) {
            kkc_set_threads(b, 1);   // 打つ側の邪魔をしない
            kkc_romance_enable(b, 1);
            ai_config(b);
            if (!init.empty()) kkc_romance_set(b, init.data(), int(init.size()));
            long long u = until, bytes = 0;
            int learned = 0;
            for (int pass = 0; pass < 4 && !ai_stop_; pass++) {   // 学んでいる間に増えた分も
                bytes = log_bytes(dir);
                const long long nu = ai_learn_log(b, dir, u, ai_stop_, learned);
                if (nu == u) break;
                u = nu;
            }
            if (!ai_stop_) ai_write(path, b, u, bytes);
            ai_bulk_learned_ = learned;
            kkc_close(b);
        }
        std::error_code rc;
        std::filesystem::remove(lock, rc);
        ai_busy_ = false;
    });
}

void Converter::ai_stop_bulk() {
    if (!ai_bulk_.joinable()) return;
    ai_stop_ = true;
    ai_bulk_.join();
}

void Converter::ai_wait() {
    if (ai_bulk_.joinable()) ai_bulk_.join();
}

float Converter::ai_learn(const u16& ctx, const u16& reading, const u16& text) {
    if (!e_ || ai_path_.empty() || ai_busy_) return -1;   // 記録から学んでいる間の分は、その記録から学ぶ
    ai_sync();   // ほかの窓で学んだ分の続きから
    const float loss = ai_step(e_, ctx, reading, text);
    if (loss >= 0) ai_save();
    return loss;
}

bool Converter::convert_once(const u16& ctx, const u16& kana, int max, Conversion& out) {
    out = Conversion();
    int want = max * 2;
    std::vector<uint16_t> buf(size_t(want) * (kana.size() * 4 + 16) + 64);
    // 変換の方式 (engine.h の use_model。1 = 毎回モデル、2 = 段階式)。計測用に SHUNTI_KKC_MODE で替えられる
    // (atoi は使わない: glibc 2.38 の __isoc23_strtol を要求して、Linux 版の動く範囲が狭まる)
    static const int mode = [] { const char* m = getenv("SHUNTI_KKC_MODE"); return m && *m >= '0' && *m <= '9' ? *m - '0' : 1; }();
    int n = kkc_convert(e_, p16(ctx), int(ctx.size()), p16(kana), int(kana.size()), want, mode, buf.data(), int(buf.size()));
    if (n == -3) return false;
    if (n <= 0) return true;
    struct Restyle {   // 返る前に 1 位の書き方を覚えた書き方に (書き方の癖。語の区切りが分かったときだけ)
        const Converter* c; const u16& ctx; Conversion& out;
        ~Restyle() { if (c->restyle_) c->style_.restyle(ctx, out); }
    } restyle{this, ctx, out};
    std::vector<u16> raw;
    for (size_t i = 0, st = 0; int(raw.size()) < n && i < buf.size(); i++) {
        if (buf[i] == 0) { raw.emplace_back(reinterpret_cast<const char16_t*>(buf.data() + st), i - st); st = i + 1; }
    }
    for (auto& c : raw) {
        if (int(out.cands.size()) >= max) break;
        if (allow_slurs || !is_slur(c)) out.cands.push_back(c);
    }
    {   // 提案
        std::vector<uint16_t> sb(8192);
        int32_t kinds[32];
        int ns = kkc_last_suggest(e_, sb.data(), int(sb.size()), kinds, 32);
        for (size_t i = 0, st = 0, k = 0; ns > 0 && int(k) < ns && i < sb.size(); i++) {
            if (sb[i] != 0) continue;
            u16 t(reinterpret_cast<const char16_t*>(sb.data() + st), i - st);
            if (allow_slurs || !is_slur(t)) { out.sugg.push_back(std::move(t)); out.sugg_kinds.push_back(kinds[k]); }
            st = i + 1;
            k++;
        }
    }
    // 。。。・・・ (ピリオドや中黒を 3 つ打った) は三点リーダー … と半角の ... にした形も 1 位のすぐ後ろに (ユーザー 2026-10-13)
    if (!out.cands.empty()) {
        u16 t = out.cands[0];
        bool hit = false;
        for (const u16& dots : {u16(u"。。。"), u16(u"・・・"), u16(u"．．．")}) {
            for (size_t at = t.find(dots); at != u16::npos; at = t.find(dots, at + 1)) {
                t.replace(at, 3, u"…");
                hit = true;
            }
        }
        if (hit && std::find(out.cands.begin(), out.cands.end(), t) == out.cands.end()) {
            out.cands.insert(out.cands.begin() + 1, t);
            // 半角の ... の形も (ユーザー 2026-10-13)
            u16 a = t;
            for (size_t at = a.find(u"…"); at != u16::npos; at = a.find(u"…", at + 3)) a.replace(at, 1, u"...");
            if (std::find(out.cands.begin(), out.cands.end(), a) == out.cands.end()) out.cands.insert(out.cands.begin() + 2, a);
        }
    }
    // 差別語で 1 位が落ちたときは区切りを使わない (区切りは元の 1 位のもの)
    if (raw.empty() || out.cands.empty() || out.cands[0] != raw[0]) return true;
    std::vector<int32_t> ends(kana.size() + 1), lens(kana.size() + 1), lids(kana.size() + 1), rids(kana.size() + 1);
    int m = kkc_last_segments(e_, ends.data(), lens.data(), int(ends.size()));
    if (m <= 0 || kkc_last_segment_pos(e_, lids.data(), rids.data(), int(lids.size())) != m) return true;
    const u16& top = raw[0];
    size_t r0 = 0, s0 = 0;
    for (int i = 0; i < m; i++) {
        size_t r1 = size_t(ends[i]), s1 = s0 + size_t(lens[i]);
        if (r1 < r0 || r1 > kana.size() || s1 > top.size()) { out.words.clear(); return true; }
        Word w{kana.substr(r0, r1 - r0), top.substr(s0, s1 - s0), lids[i], rids[i], false};
        w.pass = !w.reading.empty() && w.reading == w.surface && !is_reading_char(w.reading[0]);
        out.words.push_back(std::move(w));
        r0 = r1;
        s0 = s1;
    }
    if (r0 != kana.size() || s0 != top.size()) { out.words.clear(); return true; }
    {   // 候補の木
        constexpr int PM = 40, WC = 2048, TC = 8192;
        std::vector<double> costs(PM);
        std::vector<int32_t> cnt(PM), pe(WC), pl(WC);
        std::vector<uint16_t> tx(TC);
        int np = kkc_last_paths(e_, PM, costs.data(), cnt.data(), pe.data(), pl.data(), WC, tx.data(), TC);
        std::vector<std::pair<double, std::vector<TreeWord>>> paths;
        size_t wi = 0, ti = 0;
        for (int p = 0; p < np; p++) {
            std::vector<TreeWord> ws;
            int r = 0;
            for (int k = 0; k < cnt[size_t(p)]; k++, wi++) {
                TreeWord w;
                w.surface.assign(reinterpret_cast<const char16_t*>(tx.data() + ti), size_t(pl[wi]));
                ti += size_t(pl[wi]);
                w.rs = r;
                w.re = r = pe[wi];
                ws.push_back(std::move(w));
            }
            out.paths.push_back(ws);
            paths.push_back({costs[size_t(p)], std::move(ws)});
        }
        // 1 位の経路が候補の 1 位と同じときだけ (差別語で落ちたときなどは出さない)
        if (!paths.empty()) {
            u16 j;
            for (auto& w : paths[0].second) j += w.surface;
            if (j == top) {
                std::vector<int> cuts;
                int r = 0;
                for (auto& ph : group_phrases(out.words)) cuts.push_back(r += int(ph.reading.size()));
                out.tree = build_tree(paths, cuts);
            }
        }
    }
    return true;
}

// 経路のコストの差を確率に直す温度 (コストは辞書のコストと同じ単位。1000 ほどの差で約 e 倍)
constexpr double TREE_TEMP = 2500.0;

std::vector<TreeNode> build_tree(const std::vector<std::pair<double, std::vector<TreeWord>>>& raw, const std::vector<int>& cuts) {
    std::vector<TreeNode> out;
    if (raw.empty() || raw[0].second.empty()) return out;
    // 語を文節にまとめる: 経路ごとに、その経路の区切りのうち 1 位の文節の終わりと合う所だけで切る
    // (1 位の文節の終わりをまたぐ語がある所は、その経路だけ文節が長くなる = 区切り方で迷う所)
    std::vector<std::pair<double, std::vector<TreeWord>>> paths;
    for (auto& [c, ws] : raw) {
        std::vector<TreeWord> ch;
        TreeWord cur;
        bool open = false;
        for (auto& x : ws) {
            if (!open) { cur = TreeWord(); cur.rs = x.rs; open = true; }
            cur.surface += x.surface;
            cur.re = x.re;
            if (std::find(cuts.begin(), cuts.end(), x.re) != cuts.end()) { ch.push_back(cur); open = false; }
        }
        if (open) ch.push_back(cur);
        paths.push_back({c, std::move(ch)});
    }
    // 文節にまとめると同じになる経路は 1 つに (確率は後で足す)
    const size_t P = paths.size();
    std::vector<double> w(P);
    double sum = 0;
    for (size_t i = 0; i < P; i++) sum += w[i] = std::exp(-(paths[i].first - paths[0].first) / TREE_TEMP);
    for (auto& x : w) x /= sum;
    auto has = [&](size_t p, const TreeWord& t) {
        for (auto& x : paths[p].second)
            if (x.rs == t.rs && x.re == t.re && x.surface == t.surface) return true;
        return false;
    };
    auto bounds = [&](size_t p) {
        std::vector<int> b;
        for (auto& x : paths[p].second) b.push_back(x.re);
        return b;
    };
    const std::vector<TreeWord>& top = paths[0].second;
    const std::vector<int> b0 = bounds(0);
    // 1 位の語ごとの強さ・別の表記・前の語との結びつき
    std::vector<TreeWord> words = top;
    for (size_t k = 0; k < words.size(); k++) {
        TreeWord& t = words[k];
        double s = 0, both = 0, prev = 0;
        std::vector<std::pair<u16, double>> alt;
        for (size_t p = 0; p < P; p++) {
            const bool h = has(p, t);
            if (h) s += w[p];
            if (k > 0 && has(p, top[k - 1])) { prev += w[p]; if (h) both += w[p]; }
            for (auto& x : paths[p].second) {
                if (x.rs != t.rs || x.re != t.re || x.surface == t.surface) continue;
                auto it = std::find_if(alt.begin(), alt.end(), [&](auto& a) { return a.first == x.surface; });
                if (it == alt.end()) alt.push_back({x.surface, w[p]});
                else it->second += w[p];
            }
        }
        t.s = float(s);
        t.link = k == 0 ? 1.f : float(prev > 0 ? both / prev : s);
        std::stable_sort(alt.begin(), alt.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < alt.size() && i < 6; i++) t.alts.push_back({alt[i].first, float(alt[i].second)});
    }
    // 区切り方で迷う所: 1 位と区切りが違う経路のうち確率の高いものから、違う区間 (両方の区切りが合う所で挟む) を取る
    struct Fork { int a, b; size_t p; double mass, best; };   // best = 出す別の道 (いちばん強い経路) の確率
    std::vector<Fork> forks;
    auto in = [](const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); };
    for (size_t p = 1; p < P; p++) {
        const std::vector<int> bp = bounds(p);
        // 違う区切りの位置を 1 位の区切りの区間に広げる
        int a = -1, b = -1;
        for (int x : bp) if (!in(b0, x)) { if (a < 0 || x < a) a = x; if (x > b) b = x; }
        for (int x : b0) if (!in(bp, x) && x != b0.back()) { if (a < 0 || x < a) a = x; if (x > b) b = x; }
        if (a < 0) continue;
        int lo = 0, hi = b0.back();
        for (int x : b0) { if (x < a && in(bp, x)) lo = std::max(lo, x); if (x > b && in(bp, x)) hi = std::min(hi, x); }
        // ほかの迷う所と重なるなら足さない
        bool clash = false;
        for (auto& f : forks)
            if (!(hi <= f.a || lo >= f.b)) { if (lo == f.a && hi == f.b) f.mass += w[p]; clash = true; break; }
        if (!clash) forks.push_back({lo, hi, p, w[p], w[p]});
    }
    // 迷う所ごとに節を作る
    size_t k = 0;
    while (k < words.size()) {
        const Fork* f = nullptr;
        for (auto& x : forks)
            if (x.a == words[k].rs && x.mass >= 0.08 && x.best >= 0.06) { f = &x; break; }
        TreeNode n;
        if (!f) { n.main.push_back(words[k++]); out.push_back(std::move(n)); continue; }
        while (k < words.size() && words[k].re <= f->b) n.main.push_back(words[k++]);
        for (auto x : paths[f->p].second)
            if (x.rs >= f->a && x.re <= f->b) {
                double s = 0;
                for (size_t p = 0; p < P; p++) if (has(p, x)) s += w[p];
                x.s = float(s);
                n.fork.push_back(std::move(x));
            }
        n.p = float(std::max(0.0, 1.0 - f->mass));
        out.push_back(std::move(n));
    }
    return out;
}

Conversion Converter::convert(const u16& ctx, const u16& kana, int max) {
    Conversion out;
    if (!e_ || kana.empty()) return out;
    if (!ai_path_.empty()) ai_sync();
    if (convert_once(ctx, kana, max, out)) return out;
    // 長すぎる: 前と後ろに分けて変換し、1 位どうしをつなぐ (後ろは前の 1 位を文脈にする)
    size_t sp = split_point(kana);
    Conversion a = convert(ctx, kana.substr(0, sp), 1);
    u16 sa = a.cands.empty() ? kana.substr(0, sp) : a.cands[0];
    Conversion b = convert(ctx + sa, kana.substr(sp), 1);
    u16 sb = b.cands.empty() ? kana.substr(sp) : b.cands[0];
    out.cands.push_back(sa + sb);
    if (a.words.empty()) a.words.push_back({kana.substr(0, sp), sa});
    if (b.words.empty()) b.words.push_back({kana.substr(sp), sb});
    out.words = a.words;
    out.words.insert(out.words.end(), b.words.begin(), b.words.end());
    return out;
}

void Converter::set_right_ctx(const u16& text) {
    if (e_) kkc_set_right_ctx(e_, p16(text), int(text.size()));
}

void Converter::set_keys(const u16& keys, const std::vector<int32_t>& span_end, size_t reading_len) {
    if (e_ && span_end.size() == reading_len)
        kkc_set_keys(e_, p16(keys), int(keys.size()), span_end.data(), int(span_end.size()));
}

bool Converter::has_right_ctx() const { return e_ && kkc_has_right_ctx(e_) != 0; }

u16 Converter::latin_word(const u16& key) const {
    if (!e_ || key.empty()) return u16();
    uint16_t buf[128];
    const int n = kkc_latin_word(e_, p16(key), int(key.size()), buf, 128);
    return n > 0 ? u16(reinterpret_cast<const char16_t*>(buf), size_t(n)) : u16();
}

bool Converter::personal() const { return e_ && kkc_personal(e_); }

void Converter::set_variants(const std::string& tsv_utf8) {
    variants_ = tsv_utf8;
    style_.set_variants(tsv_utf8);
    if (!e_) return;
    const u16 t = from_utf8(tsv_utf8);
    kkc_set_variants(e_, p16(t), int(t.size()));
}

void Converter::set_marks(const std::vector<std::pair<u16, int>>& marks) {
    if (!e_) return;
    u16 all;
    std::vector<int32_t> m;
    for (auto& [s, k] : marks) { all += s; all.push_back(0); m.push_back(k); }
    kkc_set_marks(e_, p16(all), int(all.size()), m.data(), int(m.size()));
}

std::vector<std::pair<int, int>> Converter::typo_spans() const {
    std::vector<std::pair<int, int>> out;
    if (!e_) return out;
    int32_t buf[64];
    const int n = kkc_typo_spans(e_, buf, 64);
    for (int i = 0; i < n; i++) out.push_back({buf[2 * i], buf[2 * i + 1]});
    return out;
}

bool Converter::is_variant(const u16& surface) const {
    int32_t info[3];
    return e_ && !surface.empty() && kkc_variant_info(e_, p16(surface), int(surface.size()), info);
}

std::vector<u16> Converter::path_variants(const u16& reading, const u16& text) const {
    std::vector<u16> out;
    if (!e_ || reading.empty() || text.empty()) return out;
    std::vector<uint16_t> buf(text.size() * 2 + 64);
    const int n = kkc_path_variants(e_, p16(reading), int(reading.size()), p16(text), int(text.size()), buf.data(), int(buf.size()));
    u16 cur;
    for (int i = 0; i < n; i++) {
        if (buf[size_t(i)] == 0) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur.push_back(char16_t(buf[size_t(i)]));
    }
    return out;
}

void Converter::set_typo(bool on, float bias) {
    if (on == typo_ && bias == typo_bias_) return;
    typo_ = on;
    typo_bias_ = bias;
    if (e_) kkc_set_typo(e_, on ? 1 : 0, bias);
}

void Converter::set_latin(bool on) {
    latin_ = on;
    if (e_) kkc_set_latin(e_, on ? 1 : 0);
}

void Converter::set_suggest(bool on) {
    suggest_ = on;
    if (e_) kkc_set_suggest(e_, on ? 1 : 0);
}

std::vector<u16> Converter::complete(const u16& prefix, int max, int max_extra) {
    std::vector<u16> out;
    if (!e_ || prefix.empty()) return out;
    std::vector<uint16_t> buf(size_t(max) * 64 + 64);
    int n = kkc_complete(e_, p16(prefix), int(prefix.size()), max_extra, max, buf.data(), int(buf.size()));
    for (size_t i = 0, st = 0; n > 0 && int(out.size()) < n && i < buf.size(); i++) {
        if (buf[i] == 0) { out.emplace_back(reinterpret_cast<const char16_t*>(buf.data() + st), i - st); st = i + 1; }
    }
    if (!allow_slurs) out.erase(std::remove_if(out.begin(), out.end(), is_slur), out.end());
    return out;
}

std::vector<Word> Converter::path_pos(const u16& reading, const u16& text) const {
    std::vector<Word> out;
    if (!e_ || reading.empty() || text.empty()) return out;
    const size_t cap = reading.size() + 1;
    std::vector<int32_t> ends(cap), l16(cap), lids(cap), rids(cap);
    const int n = kkc_path_pos(e_, p16(reading), int(reading.size()), p16(text), int(text.size()), ends.data(), l16.data(), lids.data(), rids.data(), int(cap));
    size_t r0 = 0, s0 = 0;
    for (int i = 0; i < n; i++) {
        const size_t r1 = size_t(ends[size_t(i)]), s1 = s0 + size_t(l16[size_t(i)]);
        if (r1 > reading.size() || s1 > text.size()) return {};
        out.push_back(Word{reading.substr(r0, r1 - r0), text.substr(s0, s1 - s0), lids[size_t(i)], rids[size_t(i)], false});
        r0 = r1;
        s0 = s1;
    }
    return out;
}

int Converter::set_user_words(const std::vector<UserDict::Form>& forms, int bonus) {
    user_ = forms;
    user_bonus_ = bonus;
    if (!e_) return 0;
    kkc_user_clear(e_);
    int n = 0;
    for (auto& f : forms)
    {
        const int b = f.bonus >= 0 ? f.bonus : bonus;
        if (f.lid >= 0 && kkc_user_add_pos(e_, p16(f.reading), int(f.reading.size()), p16(f.surface), int(f.surface.size()), b, f.whole ? 1 : 0, f.lid, POS_GROUPS, POS_CLASS_COUNT)) {   // 学習した語: 選んだときの品詞の仲間の項目として (2026-11-06)。辞書に無い語は下の名詞の見本で
            n++;
            continue;
        }
        if (f.lid >= 0 && f.reading.size() < 2) continue;   // 1 字の語は品詞を決めて置けるときだけ (どの文脈でも安い 1 字の辺にしない)
        // 代表語が空 = その語そのもの (学習で選んだ語)。辞書に無ければ名詞の代表語で (AI の知らない語)
        const u16& tr = f.tmpl_reading.empty() ? f.reading : f.tmpl_reading;
        const u16& ts = f.tmpl_reading.empty() ? f.surface : f.tmpl_surface;
        int ok = kkc_user_add_like2(e_, p16(f.reading), int(f.reading.size()), p16(f.surface), int(f.surface.size()),
                                    p16(tr), int(tr.size()), p16(ts), int(ts.size()), b, f.whole ? 1 : 0);
        if (!ok && f.tmpl_reading.empty())
            ok = kkc_user_add_like2(e_, p16(f.reading), int(f.reading.size()), p16(f.surface), int(f.surface.size()),
                                    p16(u16(u"ねこ")), 2, p16(u16(u"猫")), 1, b, 0);
        n += ok;
    }
    return n;
}

std::vector<Phrase> group_phrases(const std::vector<Word>& words) {
    std::vector<Phrase> out;
    bool in_tail = false;   // いまの文節の付属語の部分に入った
    for (size_t i = 0; i < words.size(); i++) {
        const Word& w = words[i];
        bool attach = false, head = false;
        if (i > 0) {
            const Word& p = words[i - 1];
            attach = pos_class(w.lid) == '1' || is_punct(w.surface) || (pos_class(w.lid) == '4' && pos_class(p.rid) == '3');
            head = !attach && (pos_class(p.rid) == '2' || (p.pass && w.pass)) && !in_tail;
            // 辞書に無い語 (かなのまま・カタカナ) は名詞の ID なので自立語になる
        }
        if ((attach || head) && !out.empty()) {
            Phrase& ph = out.back();
            ph.reading += w.reading;
            ph.surface += w.surface;
            if (head) { ph.head_r = ph.reading.size(); ph.head_s = ph.surface.size(); }
            else in_tail = true;
        } else {
            out.push_back({w.reading, w.surface, w.reading.size(), w.surface.size()});
            in_tail = false;
        }
    }
    return out;
}

}  // namespace shunti
