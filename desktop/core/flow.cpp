#include "flow.h"

#include <algorithm>

#include "store.h"

namespace shunti {

// 10 進の整数 (atoi は使わない: glibc 2.38 の __isoc23_strtol を要求して、Linux 版の動く範囲が狭まる)
static int to_int(const char* p) {
    int sign = 1, v = 0;
    while (*p == ' ') p++;
    if (*p == '-') { sign = -1; p++; }
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return sign * v;
}


namespace {
const u16 BOUNDARY = u"。．！？!?、，\n";
const size_t MAX_CLAUSE = 40;
const size_t MAX_KEYS = 10000;   // 以前は 1500 (2026-10-24)
const int MIN_COUNT = 2;

u16 trim(const u16& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == u' ' || s[a] == u'　' || s[a] == u'\t')) a++;
    while (b > a && (s[b - 1] == u' ' || s[b - 1] == u'　' || s[b - 1] == u'\t')) b--;
    return s.substr(a, b - a);
}
}  // namespace

void SentenceFlow::load(const std::filesystem::path& path) {
    path_ = path;
    table_.clear();
    std::string data;
    if (!read_file(path, data)) return;
    size_t st = 0;
    while (st < data.size()) {
        size_t e = data.find('\n', st);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(st, e - st);
        st = e + 1;
        size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        // キー・続きの中の改行は \n と書いてある (続きは句読点の 1 字か節なので実際には入らない)
        table_[from_utf8(line.substr(0, t1))][from_utf8(line.substr(t1 + 1, t2 - t1 - 1))] = std::max(1, to_int(line.c_str() + t2 + 1));
    }
}

void SentenceFlow::save() {
    if (!dirty_ || path_.empty()) return;
    dirty_ = false;
    std::string out;
    for (const auto& kv : table_)
        for (const auto& v : kv.second) out += to_utf8(kv.first) + "\t" + to_utf8(v.first) + "\t" + std::to_string(v.second) + "\n";
    write_file_atomic(path_, out);
}

void SentenceFlow::clear() {
    table_.clear();
    dirty_ = true;
    save();
    reset();
}

void SentenceFlow::reset() {
    buf_.clear();
    prev_.clear();
}

void SentenceFlow::boundary() {
    close(u"");
    prev_.clear();
}

void SentenceFlow::backspace() {
    if (!buf_.empty()) buf_.pop_back();
    else reset();
}

void SentenceFlow::feed(const u16& text) {
    for (char16_t ch : text) {
        if (BOUNDARY.find(ch) != u16::npos) {
            close(ch == u'\n' ? u16() : u16(1, ch));
        } else {
            buf_ += ch;
            if (buf_.size() > MAX_CLAUSE * 2) buf_ = buf_.substr(buf_.size() - MAX_CLAUSE * 2);
        }
    }
}

void SentenceFlow::close(const u16& punct) {
    u16 c = trim(buf_);
    buf_.clear();
    if (c.empty() || c.size() > MAX_CLAUSE) {
        if (c.size() > MAX_CLAUSE) prev_.clear();
        return;
    }
    if (!punct.empty()) bump(u"P" + c, punct);
    if (!prev_.empty()) bump(u"N" + prev_, c);
    prev_ = c;
    dirty_ = true;
}

int SentenceFlow::suggest_use(const u16& t) const {
    auto it = table_.find(u"S" + t);
    if (it == table_.end()) return 0;
    auto j = it->second.find(u"1");
    return j == it->second.end() ? 0 : j->second;
}

void SentenceFlow::record_suggest(const u16& t) {
    bump(u"S" + t, u"1");
    dirty_ = true;
    save();
}

void SentenceFlow::bump(const u16& key, const u16& v) {
    auto it = table_.find(key);
    if (it == table_.end()) {
        if (table_.size() >= MAX_KEYS) {   // いちばん使われていないキーを 1 つ捨てる
            auto worst = table_.begin();
            int wn = 1 << 30;
            for (auto i = table_.begin(); i != table_.end(); ++i) {
                int n = 0;
                for (const auto& x : i->second) n += x.second;
                if (n < wn) { wn = n; worst = i; }
            }
            table_.erase(worst);
        }
        it = table_.emplace(key, std::map<u16, int>()).first;
    }
    it->second[v]++;
}

std::vector<u16> SentenceFlow::top(const u16& key, size_t n) const {
    std::vector<u16> out;
    auto it = table_.find(key);
    if (it == table_.end()) return out;
    std::vector<std::pair<int, u16>> v;
    for (const auto& x : it->second)
        if (x.second >= MIN_COUNT) v.push_back({x.second, x.first});
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < v.size() && i < n; i++) out.push_back(v[i].second);
    return out;
}

std::vector<u16> SentenceFlow::predict() const {
    const u16 cur = trim(buf_);
    if (!cur.empty()) return top(u"P" + cur, 2);
    std::vector<u16> out;
    if (prev_.empty()) return out;
    for (const u16& nx : top(u"N" + prev_, 3)) {
        auto p = top(u"P" + nx, 1);
        out.push_back(nx + (p.empty() ? u16() : p[0]));
    }
    return out;
}

}  // namespace shunti
