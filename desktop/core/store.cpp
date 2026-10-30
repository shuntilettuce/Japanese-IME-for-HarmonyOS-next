#include "store.h"

#include <cstdio>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>

#include "json.h"

namespace fs = std::filesystem;

namespace shunti {

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// 読めなかった (壊れた) ファイルを、上書きで消さないように別名で残す (learned.json.broken-<秒>)。
// 以前は空のまま動き、次の保存で中身を全部消していた (2026-10-24 の監査)
void keep_broken(const fs::path& p) {
    std::error_code ec;
    const auto t = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    fs::rename(p, fs::path(p.wstring() + L".broken-" + std::to_wstring(t)), ec);
}

bool write_file_atomic(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), std::streamsize(data.size()));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

namespace {
bool stamp_of(const fs::path& p, fs::file_time_type& t) {
    std::error_code ec;
    t = fs::last_write_time(p, ec);
    return !ec;
}
}  // namespace

// ---------------------------------------------------------------- 最近の学習の控え

namespace {
constexpr uintmax_t RECENT_MAX_BYTES = 256 * 1024 * 1024;

long long now_sec() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// 新しい半分だけ残す
void recent_trim(const fs::path& p) {
    std::string s;
    if (!read_file(p, s) || s.size() < RECENT_MAX_BYTES / 2) return;
    const size_t e = s.find('\n', s.size() - RECENT_MAX_BYTES / 2);
    if (e != std::string::npos) write_file_atomic(p, s.substr(e + 1));
}
}  // namespace

void recent_add(const fs::path& dir, const std::vector<std::string>& fields) {
    std::string line = std::to_string(now_sec());
    for (auto& f : fields) {
        if (f.find_first_of("\t\n") != std::string::npos) return;
        line += "\t" + f;
    }
    line += "\n";
    const fs::path p = dir / RECENT_FILE;
    {
        std::ofstream f(p, std::ios::binary | std::ios::app);
        if (!f) return;
        f.write(line.data(), std::streamsize(line.size()));
    }
    std::error_code ec;
    const uintmax_t size = fs::file_size(p, ec);
    if (!ec && size >= RECENT_MAX_BYTES) recent_trim(p);
}

void recent_drop(const fs::path& dir, const std::string& kind) {
    const fs::path p = dir / RECENT_FILE;
    std::string s;
    if (!read_file(p, s)) return;
    const std::string mark = "\t" + kind + "\t";
    std::string kept;
    for (size_t st = 0; st < s.size();) {
        size_t e = s.find('\n', st);
        if (e == std::string::npos) e = s.size();
        const size_t tab = s.find('\t', st);
        if (tab == std::string::npos || s.compare(tab, mark.size(), mark) != 0) kept.append(s, st, e + 1 - st);
        st = e + 1;
    }
    write_file_atomic(p, kept);
}

// ---------------------------------------------------------------- 学習

void Learning::note_commit(const u16& reading, const u16& surface, const u16& ctx) {
    u16 c = ctx;
    for (auto& ch : c) if (ch == u'\t' || ch == u'\n' || ch == u'\r') ch = u' ';
    recent_add(file_.parent_path(), {"C", to_utf8(reading), to_utf8(surface), to_utf8(c)});
}

void Learning::refresh() {
    fs::file_time_type t{};
    bool exists = stamp_of(file_, t);
    if (loaded_ && (!exists || t == stamp_)) return;
    loaded_ = true;
    stamp_ = t;
    entries_.clear();
    version_++;
    std::string s;
    if (!exists || !read_file(file_, s)) return;
    json::Value v;
    if (!json::Parser(s).parse(v) || v.type != json::Value::Object) { keep_broken(file_); return; }
    bool weighted = false;
    for (auto& r : v.obj) {
        if (r.first == "@w") { weighted = true; continue; }
        if (r.second.type != json::Value::Object) continue;
        Entry e{from_utf8(r.first), {}};
        for (auto& c : r.second.obj) {
            if (c.second.type != json::Value::Number) continue;
            if (c.first == "@day") e.day = int(c.second.num);   // 最後に選んだ日 (前の版のファイルには無い)
            else e.counts.push_back({from_utf8(c.first), int(c.second.num)});
        }
        if (!e.counts.empty()) entries_.push_back(std::move(e));
    }
    // 前の版の回数は手で選んだ回数そのものなので、今の数え方 (2 回分) にそろえる
    if (!weighted)
        for (auto& e : entries_)
            for (auto& c : e.counts) if (c.second > 0) c.second *= 2;
    reindex();
}

void Learning::reindex() {
    idx_.clear();
    for (size_t i = 0; i < entries_.size(); i++) idx_[entries_[i].reading] = i;
}

int Learning::today() {
    return int(std::chrono::duration_cast<std::chrono::hours>(std::chrono::system_clock::now().time_since_epoch()).count() / 24);
}

int Learning::last_day(const u16& reading) {
    refresh();
    Entry* e = find(reading);
    return e ? e->day : 0;
}

std::vector<std::pair<u16, int>> Learning::marks() {
    refresh();
    struct Acc { int n = 0, day = 0; };
    std::unordered_map<u16, Acc> by;
    for (auto& e : entries_) {
        int kana = 0;   // かなのまま確定した分は、同じ読みの漢字の印から引く (かなの方が多ければ印を付けない)
        for (auto& c : e.counts) if (c.first == e.reading) kana = std::max(c.second, 0);
        for (auto& c : e.counts) {
            const int n = c.second - kana;
            if (n <= 0) continue;
            u16 r = e.reading, s = c.first;
            while (!r.empty() && !s.empty() && r.back() == s.back() && r.back() >= 0x3041 && r.back() <= 0x3096) { r.pop_back(); s.pop_back(); }
            if (s.empty()) continue;
            Acc& a = by[s];
            a.n += (n + 1) / 2;   // 印の段階は手で選んだ回数で数える
            a.day = std::max(a.day, e.day);
        }
    }
    const int t = today();
    std::vector<std::pair<u16, int>> out;
    for (auto& [s, a] : by) {
        const int c = a.n == 1 ? 1 : a.n == 2 ? 2 : a.n <= 4 ? 3 : a.n <= 9 ? 4 : a.n <= 19 ? 5 : 6;
        const int age = a.day > 0 ? t - a.day : 9999;   // 日付の無い (前の版の) 記録は古いものとして
        const int r = age <= 7 ? 0 : age <= 30 ? 1 : age <= 183 ? 2 : 3;
        out.push_back({s, 1 + (c - 1) * 4 + r});
    }
    return out;
}

void Learning::save() {
    version_++;
    // 手で選ぶと 2 回分と数えた回数。読みの形で書く (数字だと、開いたままのアプリの古い版が消して書き戻し、次に 2 倍になった)
    std::string o = "{\"@w\":{\"@w\":2}";
    for (auto& e : entries_) {
        o += ",";
        o += json::quote(to_utf8(e.reading)) + ":{";
        for (size_t i = 0; i < e.counts.size(); i++) {
            if (i) o += ",";
            o += json::quote(to_utf8(e.counts[i].first)) + ":" + std::to_string(e.counts[i].second);
        }
        if (e.day > 0) o += ",\"@day\":" + std::to_string(e.day);
        o += "}";
    }
    o += "}";
    if (write_file_atomic(file_, o)) stamp_of(file_, stamp_);
}

Learning::Entry* Learning::find(const u16& reading) {
    auto it = idx_.find(reading);
    return it == idx_.end() ? nullptr : &entries_[it->second];
}

Learning::Undo Learning::confirm(const u16& reading, const u16& surface) {
    refresh();
    Entry* e = find(reading);
    if (!e) return {};
    return record(reading, surface, 1);
}

Learning::Undo Learning::record(const u16& reading, const u16& surface, int add) {
    // かなのままの表記も数える。数えないと、かなで打ち続けても 1 回選んだ漢字に勝てない (ユーザー 2026-10-31)
    if (reading.empty() || surface.empty()) return {};
    refresh();
    Undo undo{reading, surface};
    Entry* e = find(reading);
    if (e) {
        undo.day = e->day;
        for (auto& c : e->counts) if (c.first == surface) undo.count = c.second;
    }
    recent_add(file_.parent_path(), {"L", to_utf8(reading), to_utf8(surface), std::to_string(undo.count), std::to_string(undo.day)});
    if (!e) {
        if (entries_.size() >= MAX_READINGS) {
            // 回数が少なく長く使っていない読みを捨てる (回数 1 は 30 日ぶん。20 回以上は同じ扱い)
            const int t = today();
            auto score = [&](const Entry& x) {
                int m = 0;
                for (auto& c : x.counts) m = std::max(m, c.second);
                return std::min(m / 2, 20) * 30 - (x.day > 0 ? t - x.day : 365);
            };
            auto weakest = std::min_element(entries_.begin(), entries_.end(), [&](const Entry& a, const Entry& b) { return score(a) < score(b); });
            entries_.erase(weakest);
            reindex();
        }
        entries_.push_back({reading, {}});
        idx_[reading] = entries_.size() - 1;
        e = &entries_.back();
    }
    e->day = today();
    for (auto& c : e->counts) {
        if (c.first == surface) { c.second = c.second < 0 ? add : c.second + add; save(); return undo; }   // 消した表記を自分で選んだら戻す
    }
    // 表記は 5 つまで。数えるのは選んだ表記だけ (消した表記 = 回数 -1 は枠を食わない。2026-10-24 の監査)。
    // 埋まっていたら、いちばん少なく選んだ表記と入れ替える (以前は 6 つ目を黙って覚えなかった)
    size_t n = 0;
    for (auto& c : e->counts) n += c.second > 0;
    if (n >= MAX_SURFACES) {
        auto weakest = e->counts.end();
        for (auto it = e->counts.begin(); it != e->counts.end(); ++it)
            if (it->second > 0 && (weakest == e->counts.end() || it->second < weakest->second)) weakest = it;
        if (weakest != e->counts.end()) e->counts.erase(weakest);
    }
    e->counts.push_back({surface, add});
    save();
    return undo;
}

void Learning::restore(const std::vector<Undo>& us) {
    refresh();
    bool changed = false;
    for (const Undo& u : us) {
        if (u.reading.empty()) continue;
        Entry* e = find(u.reading);
        if (!e) {
            if (u.count == 0) continue;
            entries_.push_back({u.reading, {}});
            idx_[u.reading] = entries_.size() - 1;
            e = &entries_.back();
        }
        auto it = std::find_if(e->counts.begin(), e->counts.end(), [&](auto& c) { return c.first == u.surface; });
        if (it != e->counts.end() && it->second < 0 && !u.hidden) continue;
        if (u.count != 0) {
            if (it != e->counts.end()) it->second = u.count;
            else e->counts.push_back({u.surface, u.count});
        } else if (it != e->counts.end()) {
            e->counts.erase(it);
        }
        e->day = u.day;
        changed = true;
        if (e->counts.empty()) {
            entries_.erase(entries_.begin() + (e - entries_.data()));
            reindex();
        }
    }
    if (changed) save();
}

void Learning::hide(const u16& reading, const u16& surface, bool keep_recent) {
    if (reading.empty() || surface.empty()) return;
    refresh();
    Entry* e = find(reading);
    int prev = 0;
    if (e) for (auto& c : e->counts) if (c.first == surface) prev = c.second;
    if (keep_recent) recent_add(file_.parent_path(), {"H", to_utf8(reading), to_utf8(surface), std::to_string(prev), std::to_string(e ? e->day : 0)});
    if (!e) {
        entries_.push_back({reading, {}});
        idx_[reading] = entries_.size() - 1;
        e = &entries_.back();
    }
    for (auto& c : e->counts)
        if (c.first == surface) { c.second = -1; save(); return; }
    e->counts.push_back({surface, -1});   // 選んだ回数の上限 (MAX_SURFACES) とは別に覚える
    save();
}

bool Learning::hidden(const u16& reading, const u16& surface) {
    refresh();
    Entry* e = find(reading);
    if (!e) return false;
    for (auto& c : e->counts)
        if (c.first == surface) return c.second < 0;
    return false;
}

void Learning::forget(const u16& reading, const u16& surface) {
    refresh();
    for (size_t i = 0; i < entries_.size(); i++) {
        auto& cs = entries_[i].counts;
        if (entries_[i].reading != reading) continue;
        auto it = std::find_if(cs.begin(), cs.end(), [&](auto& c) { return c.first == surface; });
        if (it == cs.end()) return;
        cs.erase(it);
        if (cs.empty()) {
            entries_.erase(entries_.begin() + long(i));
            reindex();
        }
        save();
        return;
    }
}

std::vector<u16> Learning::apply_order(const u16& reading, const std::vector<u16>& cands) {
    refresh();
    Entry* e = find(reading);
    if (!e) return cands;
    auto count = [&](const u16& s) { for (auto& c : e->counts) if (c.first == s) return c.second; return 0; };
    // TOP_MIN_COUNT 回以上の表記は先頭へ (回数の多い順)。手で選ぶと 2 回分、選び直さずに確定すると 1 回分足すので、
    // 選び直した表記が先頭に上がり、そのあとモデルの 1 位のまま確定した回数が多ければ元に戻る
    std::vector<u16> boosted, rest;
    for (auto& c : cands) {
        const int n = count(c);
        if (n < 0) continue;   // 消した表記
        (n >= TOP_MIN_COUNT ? boosted : rest).push_back(c);
    }
    std::stable_sort(boosted.begin(), boosted.end(), [&](const u16& a, const u16& b) { return count(a) > count(b); });
    boosted.insert(boosted.end(), rest.begin(), rest.end());
    // 英カタカナ辞書の英単語 (全部小文字: いんじぇくしょん → injection) は、学習で上がっても 2 位まで (1 位はカタカナのまま)
    auto english = [](const u16& s) {
        if (s.size() < 2) return false;
        for (char16_t c : s) if (!((c >= u'a' && c <= u'z') || c == u'-')) return false;
        return true;
    };
    bool kana = false;
    for (char16_t c : reading) kana |= c >= 0x80;
    if (kana && boosted.size() >= 2 && english(boosted[0])) std::swap(boosted[0], boosted[1]);
    return boosted;
}

std::vector<u16> Learning::completions(const u16& prefix, size_t max) {
    std::vector<u16> out;
    if (prefix.empty()) return out;
    refresh();
    std::vector<std::pair<u16, int>> all;
    for (auto& e : entries_)
        if (e.reading.size() > prefix.size() && e.reading.compare(0, prefix.size(), prefix) == 0)
            for (auto& c : e.counts) if (c.second >= 4) all.push_back(c);   // 手で 2 回分以上だけ (1 回きりの長い文が続きに出ないように。スマホ版と同じ)
    std::stable_sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& c : all) {
        if (out.size() >= max) break;
        if (std::find(out.begin(), out.end(), c.first) == out.end()) out.push_back(c.first);
    }
    return out;
}

std::vector<std::pair<u16, int>> Learning::counts(const u16& reading) {
    refresh();
    Entry* e = find(reading);
    return e ? e->counts : std::vector<std::pair<u16, int>>{};
}

u16 Learning::top(const u16& reading) {
    refresh();
    Entry* e = find(reading);
    if (!e) return u16();
    u16 best;
    int n = TOP_MIN_COUNT - 1;
    for (auto& c : e->counts) if (c.second > n) { n = c.second; best = c.first; }
    return best;
}

std::vector<Learning::Strong> Learning::strong(int min) {
    refresh();
    std::vector<Strong> out;
    for (auto& e : entries_)
        for (auto& c : e.counts)
            if (c.second >= min) out.push_back({e.reading, c.first, c.second});
    return out;
}

void Learning::clear() {
    entries_.clear();
    idx_.clear();
    loaded_ = true;
    save();
    std::error_code ec;
    fs::remove(file_.string() + ".pre-kana", ec);   // release_kana の前の控えも学習なので一緒に消す
}

int Learning::release_kana(const std::function<bool(const u16&)>& kana_first) {
    refresh();
    int fixed = 0;
    for (auto& e : entries_) {
        if (e.reading.size() > 5) continue;   // 入力ログで負けた読みは 5 字まで
        int best = 0, kana = 0;
        for (auto& c : e.counts) {
            if (c.first == e.reading) kana = c.second;
            else best = std::max(best, c.second);
        }
        if (best < TOP_MIN_COUNT || kana > best || !kana_first(e.reading)) continue;
        for (auto& c : e.counts) if (c.first != e.reading && c.second > 0) c.second = 0;   // 0 なら、古い版が @w を落として回数が 2 倍になっても 1 位に戻らない
        fixed++;
    }
    if (fixed) save();
    return fixed;
}

// ---------------------------------------------------------------- ユーザー辞書

namespace {
struct Tmpl { const char16_t* r; const char16_t* s; };
// 品詞を写す代表語 (どれも辞書にあることを確かめてある。Android 版と同じ)
struct Godan { char16_t last; const char16_t* r; const char16_t* s; const char16_t* sufs; };
const Godan GODAN[] = {
    {u'う', u"かう", u"買う", u"わいうえおっ"}, {u'く', u"かく", u"書く", u"かきくけこい"},
    {u'ぐ', u"およぐ", u"泳ぐ", u"がぎぐげごい"}, {u'す', u"はなす", u"話す", u"さしすせそ"},
    {u'つ', u"まつ", u"待つ", u"たちつてとっ"}, {u'ぬ', u"しぬ", u"死ぬ", u"なにぬねのん"},
    {u'ぶ', u"あそぶ", u"遊ぶ", u"ばびぶべぼん"}, {u'む', u"よむ", u"読む", u"まみむめもん"},
    {u'る', u"はしる", u"走る", u"らりるれろっ"},
};
const char16_t* ICHIDAN[] = {u"", u"る", u"れ", u"ろ", u"よ"};                          // 代表語 食べる
const char16_t* ADJ[] = {u"い", u"く", u"かっ", u"けれ", u"かろ", u"き", u"さ", u"そう"};   // 代表語 高い

const Godan* godan_of(char16_t last) {
    for (auto& g : GODAN) if (g.last == last) return &g;
    return nullptr;
}
bool ends_with(const u16& s, const u16& t) { return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0; }
}  // namespace

bool UserDict::refresh() {
    fs::file_time_type t{};
    bool exists = stamp_of(file_, t);
    if (loaded_ && (!exists || t == stamp_)) return false;
    loaded_ = true;
    stamp_ = t;
    entries_.clear();
    version_++;
    std::string s;
    if (!exists || !read_file(file_, s)) return true;
    json::Value v;
    if (!json::Parser(s).parse(v) || v.type != json::Value::Array) { keep_broken(file_); return true; }
    for (auto& o : v.arr) {
        if (o.type != json::Value::Object) continue;
        Entry e{from_utf8(o.get_str("r")), from_utf8(o.get_str("w")), o.get_str("p", "noun"), o.get_str("g")};
        if (!e.reading.empty() && !e.word.empty()) entries_.push_back(std::move(e));
    }
    return true;
}

// 読みは英数字・記号も使える (＠ほーむ・ｗｗ)。打った読みが全角でも半角でも当たるように、比べるときは半角にそろえる
static u16 reading_key(const u16& r) { return to_halfwidth_ascii(r); }

std::vector<u16> UserDict::lookup(const u16& reading) const {
    std::vector<u16> out;
    const u16 k = reading_key(reading);
    for (auto& e : entries_) if (reading_key(e.reading) == k) out.push_back(e.word);
    return out;
}

// MS-IME の品詞名 -> 内部の品詞 (pos, group)。知らない名前は名詞
std::pair<std::string, std::string> UserDict::pos_from_label(const u16& label) {
    const std::string l = to_utf8(label);
    auto has = [&](const char* s) { return l.find(s) != std::string::npos; };
    if (has("短縮")) return {"abbrev", ""};
    if (has("顔文字")) return {"kaomoji", ""};
    if (has("人名") || l == "姓" || l == "名") return {"person", ""};
    if (has("地名")) return {"place", ""};
    if (has("組織") || has("固有名詞")) return {"org", ""};
    if (has("感動詞")) return {"interjection", ""};   // 「動詞」を含むので動詞より先に (ありがとう が五段にされていた)
    if (has("形容動詞")) return {"noun", ""};   // 有名・静か: 名詞と同じに扱う (「動詞」に当てない)
    if (has("一段")) return {"verb", "ichidan"};
    if (has("動詞")) return {"verb", "godan"};
    if (has("形容詞")) return {"adjective", ""};
    if (has("副詞")) return {"adverb", ""};
    return {"noun", ""};
}

u16 UserDict::export_text() const {
    auto label = [](const Entry& e) -> const char16_t* {
        if (e.pos == "person") return u"人名";
        if (e.pos == "place") return u"地名その他";
        if (e.pos == "org") return u"固有名詞";
        if (e.pos == "abbrev") return u"短縮よみ";
        if (e.pos == "kaomoji") return u"顔文字";
        if (e.pos == "verb") return e.group == "ichidan" ? u"動詞一段" : u"動詞五段";
        if (e.pos == "adjective") return u"形容詞";
        if (e.pos == "adverb") return u"副詞";
        if (e.pos == "interjection") return u"感動詞";
        return u"名詞";
    };
    u16 o = u"!Microsoft IME Dictionary Tool\r\n!shunti IME から書き出し\r\n";
    for (auto& e : entries_) o += e.reading + u"\t" + e.word + u"\t" + label(e) + u"\r\n";
    return o;
}

// まとめて登録: MS-IME の「一覧の出力」(読み<TAB>語句<TAB>品詞、! で始まる行は説明) や、読み,語句,品詞 の CSV。
// 返り値: (登録できた数, 登録できなかった行の説明)
std::pair<int, std::vector<std::string>> UserDict::import_text(const u16& text) {
    int ok = 0, line_no = 0;
    std::vector<std::string> bad;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find(u'\n', i);
        if (j == u16::npos) j = text.size();
        u16 line = text.substr(i, j - i);
        i = j + 1;
        line_no++;
        if (!line.empty() && line.back() == u'\r') line.pop_back();
        if (!line.empty() && line[0] == 0xFEFF) line.erase(0, 1);
        if (line.empty() || line[0] == u'!' || line[0] == u'#') continue;
        const char16_t sep = line.find(u'\t') != u16::npos ? u'\t' : u',';
        std::vector<u16> f;
        size_t a = 0;
        while (true) {
            size_t b = line.find(sep, a);
            f.push_back(line.substr(a, b == u16::npos ? u16::npos : b - a));
            if (b == u16::npos) break;
            a = b + 1;
        }
        if (f.size() < 2) { bad.push_back(std::to_string(line_no) + " 行目: 読みと語句がありません"); continue; }
        auto pg = pos_from_label(f.size() >= 3 ? f[2] : u16());
        std::string err = add(f[0], f[1], pg.first, pg.second);
        if (err.empty()) ok++;
        else if (err != "もう登録してあります") bad.push_back(std::to_string(line_no) + " 行目 (" + to_utf8(f[0]) + "): " + err);
    }
    return {ok, bad};
}

// ---------------------------------------------------------------- 書き方の記録 (M5)

void StyleLog::refresh() {
    fs::file_time_type t{};
    bool exists = stamp_of(file_, t);
    if (loaded_ && (!exists || t == stamp_)) return;
    loaded_ = true;
    stamp_ = t;
    m_.clear();
    version_++;
    std::string s;
    if (!exists || !read_file(file_, s)) return;
    json::Value v;
    if (!json::Parser(s).parse(v) || v.type != json::Value::Object) { keep_broken(file_); return; }
    const json::Value* sv = nullptr;
    for (auto& a : v.obj) if (a.first == "s" && a.second.type == json::Value::Object) sv = &a.second;
    if (sv) {   // 今の形: {"v":2,"s":{表記:[丁寧の重み, 崩れの重み, 回数]}}
        for (auto& c : sv->obj) {
            if (c.second.type != json::Value::Array || c.second.arr.size() != 3) continue;
            StyleModel::Count k;
            k.w[0] = float(c.second.arr[0].num);
            k.w[1] = float(c.second.arr[1].num);
            k.n = int(c.second.arr[2].num);
            if (k.n > 0) m_[from_utf8(c.first)] = k;
        }
        return;
    }
    for (auto& a : v.obj) {   // 古い形 (アプリ → 表記 → 回数): 合計を、文の種類が分からない (半々) として読む
        if (a.second.type != json::Value::Object) continue;
        for (auto& c : a.second.obj) {
            if (c.second.type != json::Value::Number || c.second.num <= 0) continue;
            auto& k = m_[from_utf8(c.first)];
            k.n += int(c.second.num);
            k.w[0] = k.w[1] = float(k.n) * 0.70710678f;
        }
    }
}

void StyleLog::save() {
    version_++;
    std::string o = "{\"v\":2,\"s\":{";
    bool f = true;
    char buf[64];
    for (auto& [s, k] : m_) {
        if (!f) o += ",";
        f = false;
        snprintf(buf, sizeof buf, ":[%.3f,%.3f,%d]", double(k.w[0]), double(k.w[1]), k.n);
        o += json::quote(to_utf8(s)) + buf;
    }
    o += "}}";
    if (write_file_atomic(file_, o)) stamp_of(file_, stamp_);
}

void StyleLog::record(const u16& surface, const std::array<float, 2>& w) {
    if (surface.empty()) return;
    refresh();
    if (!m_.count(surface) && m_.size() >= MAX_SURFACES) {   // あふれたら回数のいちばん少ない表記を捨てる
        auto low = std::min_element(m_.begin(), m_.end(), [](auto& a, auto& b) { return a.second.n < b.second.n; });
        m_.erase(low);
    }
    auto& k = m_[surface];
    if (k.n >= 1000000) return;
    k.w[0] += w[0];
    k.w[1] += w[1];
    k.n++;
    char a[32], b[32];
    snprintf(a, sizeof a, "%.3f", double(w[0]));
    snprintf(b, sizeof b, "%.3f", double(w[1]));
    recent_add(file_.parent_path(), {"S", to_utf8(surface), a, b});
    save();
}

void StyleLog::unrecord(const std::vector<std::pair<u16, std::array<float, 2>>>& items) {
    refresh();
    bool changed = false;
    for (auto& [s, w] : items) {
        auto it = m_.find(s);
        if (it == m_.end()) continue;
        changed = true;
        auto& k = it->second;
        if (--k.n <= 0) { m_.erase(it); continue; }
        k.w[0] = std::max(0.f, k.w[0] - w[0]);
        k.w[1] = std::max(0.f, k.w[1] - w[1]);
    }
    if (changed) save();
}

void StyleLog::clear() {
    refresh();
    m_.clear();
    save();
}

// 英字の語の綴り: 英字・数字だけ (間の空白は除く) を小文字にしたもの。英字・数字・空白以外を含めば空
u16 latin_key(const u16& w) {
    u16 k;
    for (char16_t c : w) {
        if ((c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9')) k += c;
        else if (c >= u'A' && c <= u'Z') k += char16_t(c + 32);
        else if (c != u' ') return u16();
    }
    return k;
}

std::vector<std::pair<u16, u16>> UserDict::latin_words() const {
    std::vector<std::pair<u16, u16>> out;
    for (auto& e : entries_) {
        u16 k = latin_key(e.word);
        if (k.size() >= 2) out.push_back({k, e.word});
    }
    return out;
}

std::string UserDict::add(const u16& reading0, const u16& word0, const std::string& pos, const std::string& group) {
    refresh();
    auto trim = [](u16 s) {
        while (!s.empty() && (s.back() == u' ' || s.back() == 0x3000)) s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == u' ' || s[i] == 0x3000)) i++;
        return s.substr(i);
    };
    u16 r = to_hiragana(trim(reading0)), w = trim(word0);
    // 英字の語 (ShuntiLettuce・shuntIME): 読みを空にすると、打ったままの綴り (小文字・空白なし) を読みにする。
    // ローマ字で英字のまま打つと出る (エンジンの英字の辺・打ち切った綴りが同じなら候補の 2 番目。2026-10-23)
    std::string p = pos;
    if (p == "latin" || (r.empty() && latin_key(w).size() >= 2)) {
        if (latin_key(w).size() < 2) return "英字の語は、英字・数字 (と語の間の空白) で 2 字以上入れてください (例: ShuntiLettuce)";
        r = latin_key(w);
        p = "latin";
    }
    if (r.empty() || w.empty()) return "読みと単語を入れてください";
    // 読みには、ひらがなのほか英数字・記号も使える (＠ほーむ・ｗｗ・１ばん)。漢字は不可 (打てない)
    for (char16_t c : r) {
        if ((c >= 0x4E00 && c <= 0x9FFF) || c == 0x3005) return "読みに漢字は使えません。ひらがなで入れてください";
        if (c < 0x20) return "読みに使えない字があります";
    }
    if (pos == "verb") {
        if (group == "ichidan") {
            if (!ends_with(r, u"る") || !ends_with(w, u"る")) return "一段の動詞は「る」で終わる形で入れてください (例: たべる / 食べる)";
        } else if (!godan_of(r.back()) || w.back() != r.back()) {
            return "動詞は終止形で、読みと単語の最後を同じかなにしてください (例: ぐぐる / ググる)";
        }
    } else if (pos == "adjective") {
        if (!ends_with(r, u"い") || !ends_with(w, u"い")) return "形容詞は「い」で終わる形で入れてください (例: えもい / エモい)";
    }
    for (auto& e : entries_) if (e.reading == r && e.word == w) return "もう登録してあります";
    entries_.push_back({r, w, p, p == "verb" ? group : ""});
    save();
    return "";
}

void UserDict::remove(const u16& reading, const u16& word) {
    refresh();
    auto it = std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.reading == reading && e.word == word; });
    if (it == entries_.end()) return;
    entries_.erase(it, entries_.end());
    save();
}

void UserDict::save() {
    version_++;
    // 1 語 1 行 (手で読み書きしやすく。1 行に全部つなげていて見にくかった。ブログの報告 2026-10-18)
    std::string o = "[\n";
    for (size_t i = 0; i < entries_.size(); i++) {
        auto& e = entries_[i];
        if (i) o += ",\n";
        o += "  {\"r\":" + json::quote(to_utf8(e.reading)) + ",\"w\":" + json::quote(to_utf8(e.word)) +
             ",\"p\":" + json::quote(e.pos) + ",\"g\":" + json::quote(e.group) + "}";
    }
    o += entries_.empty() ? "]\n" : "\n]\n";
    if (write_file_atomic(file_, o)) stamp_of(file_, stamp_);
}

std::vector<UserDict::Form> UserDict::engine_forms() const {
    std::vector<Form> out;
    for (auto& e : entries_) {
        const u16& r = e.reading;
        const u16& w = e.word;
        auto stem = [&](const u16& tr, const u16& ts, const u16& suf) {
            out.push_back({r.substr(0, r.size() - 1) + suf, w.substr(0, w.size() - 1) + suf,
                           tr.substr(0, tr.size() - 1) + suf, ts.substr(0, ts.size() - 1) + suf});
        };
        if (e.pos == "verb") {
            if (e.group == "ichidan") {
                for (auto s : ICHIDAN) stem(u"たべる", u"食べる", s);
            } else if (const Godan* g = godan_of(r.back())) {
                for (const char16_t* p = g->sufs; *p; p++) stem(g->r, g->s, u16(1, *p));
            }
        } else if (e.pos == "adjective") {
            for (auto s : ADJ) stem(u"たかい", u"高い", s);
        } else {
            // 品詞を写す代表語。組織名は地名と同じ固有名詞、副詞・感動詞はそれぞれの代表語、それ以外 (短縮よみ・顔文字) は名詞
            Tmpl t = e.pos == "person" ? Tmpl{u"やまだ", u"山田"}
                   : e.pos == "place" || e.pos == "org" ? Tmpl{u"とうきょう", u"東京"}
                   : e.pos == "adverb" ? Tmpl{u"とても", u"とても"}
                   : e.pos == "interjection" ? Tmpl{u"こんにちは", u"こんにちは"}
                   : Tmpl{u"ねこ", u"猫"};
            const bool whole = e.pos == "abbrev";   // 短縮よみは、その読みだけを打ったときだけ
            out.push_back({r, w, t.r, t.s, whole});
            // 読みに英数字・記号があれば、全角と半角の両方で引けるように
            const u16 rh = to_halfwidth_ascii(r), rf = to_fullwidth_ascii(r);
            if (rh != r) out.push_back({rh, w, t.r, t.s, whole});
            if (rf != r && rf != rh) out.push_back({rf, w, t.r, t.s, whole});
        }
    }
    return out;
}

}  // namespace shunti
