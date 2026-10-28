#include "release_info.h"

#include <cstdint>

namespace shunti {

namespace {

const char* PAGE = "https://github.com/shuntilettuce/Japanese-IME-for-HarmonyOS-next/releases/tag/";

// JSON の中の "key":"値" を pos から探す (GitHub の返事だけを読むので、簡単な探し方で足りる。値に \" を含まないもの)
bool next_value(const std::string& j, const std::string& key, size_t& pos, std::string& out) {
    const std::string k = "\"" + key + "\"";
    size_t p = j.find(k, pos);
    if (p == std::string::npos) return false;
    p = j.find('"', j.find(':', p + k.size()) + 1);
    if (p == std::string::npos) return false;
    size_t e = j.find('"', p + 1);
    if (e == std::string::npos) return false;
    out = j.substr(p + 1, e - p - 1);
    pos = e + 1;
    return true;
}

// JSON の文字列 "..." を pos (最初の " の位置) から読む (\n・\"・\uXXXX を戻す)。本文は改行や引用符を含むので next_value では読めない
std::string json_string(const std::string& j, size_t pos) {
    std::string o;
    auto put = [&](uint32_t c) {
        if (c < 0x80) o += char(c);
        else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
        else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 0x3F)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
    };
    auto hex4 = [&](size_t at) {
        uint32_t v = 0;
        for (size_t k = at; k < at + 4 && k < j.size(); k++) {
            const char c = j[k];
            v = v * 16 + uint32_t(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
        }
        return v;
    };
    for (size_t i = pos + 1; i < j.size() && j[i] != '"'; i++) {
        if (j[i] != '\\' || i + 1 >= j.size()) { o += j[i]; continue; }
        const char e = j[++i];
        if (e == 'n') o += '\n';
        else if (e == 't') o += ' ';
        else if (e == 'r') {}
        else if (e == 'u') {
            uint32_t c = hex4(i + 1);
            i += 4;
            if (c >= 0xD800 && c < 0xDC00 && i + 6 < j.size() && j[i + 1] == '\\' && j[i + 2] == 'u') {
                c = 0x10000 + ((c - 0xD800) << 10) + (hex4(i + 3) - 0xDC00);
                i += 6;
            }
            put(c);
        } else o += e;
    }
    return o;
}

std::vector<int> parts(const std::string& v) {
    std::vector<int> o;
    int x = 0;
    bool any = false;
    for (char c : v) {
        if (c >= '0' && c <= '9') { x = x * 10 + (c - '0'); any = true; }
        else if (c == '.') { o.push_back(x); x = 0; any = false; }
        else break;
    }
    if (any) o.push_back(x);
    return o;
}

}  // namespace

std::vector<std::string> release_change_lines(const std::string& md) {
    std::vector<std::string> out;
    const std::string tag = "<!-- \xE6\x9B\xB4\xE6\x96\xB0\xE3\x81\xAE\xE7\xA2\xBA\xE8\xAA\x8D";   // "<!-- 更新の確認"
    const size_t hb = md.find(tag);
    if (hb != std::string::npos) {
        const size_t he = md.find("-->", hb);
        const std::string inner = md.substr(hb + tag.size(), he == std::string::npos ? std::string::npos : he - hb - tag.size());
        size_t st = 0;
        while (st < inner.size() && out.size() < 8) {
            size_t en = inner.find('\n', st);
            if (en == std::string::npos) en = inner.size();
            std::string line = inner.substr(st, en - st);
            st = en + 1;
            while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
            if (line.rfind("- ", 0) == 0) line = line.substr(2);
            else if (line.rfind("\xE3\x83\xBB", 0) == 0) line = line.substr(3);   // ・
            if (!line.empty()) out.push_back(line);
        }
        return out;
    }
    size_t st = 0;
    while (st < md.size() && out.size() < 8) {
        size_t en = md.find('\n', st);
        if (en == std::string::npos) en = md.size();
        std::string line = md.substr(st, en - st);
        st = en + 1;
        if (line.rfind("## ", 0) == 0 && !out.empty()) break;   // 次の大きな見出し (導入方法) まで
        if (line.rfind("- ", 0) != 0) continue;
        line = line.substr(2);
        if (line.rfind("**", 0) == 0) {
            const size_t e = line.find("**", 2);
            if (e != std::string::npos) line = line.substr(2, e - 2);
        }
        std::string t;
        for (char c : line) if (c != '`' && c != '*' && c != '\r') t += c;
        out.push_back(t);
    }
    return out;
}

bool version_older(const std::string& a, const std::string& b) {
    std::vector<int> x = parts(a), y = parts(b);
    while (x.size() < y.size()) x.push_back(0);
    while (y.size() < x.size()) y.push_back(0);
    return x < y;
}

ReleaseInfo latest_release(const std::string& body, const std::string& tag_prefix, const std::string& asset_prefix,
                           const std::string& asset_suffix) {
    ReleaseInfo best;
    // Release ごとに tag_name・添付の browser_download_url・本文 body が並ぶ (新しい順)
    size_t pos = 0;
    std::string tag;
    while (next_value(body, "tag_name", pos, tag)) {
        const size_t next_tag = body.find("\"tag_name\"", pos);
        if (tag.rfind(tag_prefix, 0) != 0) continue;
        const std::string ver = tag.substr(tag_prefix.size());
        if (!best.version.empty() && !version_older(best.version, ver)) continue;
        const std::string block = body.substr(pos, next_tag == std::string::npos ? std::string::npos : next_tag - pos);
        size_t q = 0;
        std::string url, asset;
        while (next_value(block, "browser_download_url", q, url)) {
            const size_t slash = url.rfind('/');
            const std::string name = slash == std::string::npos ? url : url.substr(slash + 1);
            if (name.rfind(asset_prefix, 0) == 0 && name.size() >= asset_suffix.size() &&
                name.compare(name.size() - asset_suffix.size(), asset_suffix.size(), asset_suffix) == 0)
                asset = url;
        }
        if (asset.empty()) continue;
        best.version = ver;
        best.asset_url = asset;
        best.page_url = PAGE + tag;
        best.changes.clear();
        const size_t b = block.find("\"body\"");
        if (b != std::string::npos) {
            const size_t q0 = block.find('"', block.find(':', b + 6) + 1);
            if (q0 != std::string::npos) best.changes = release_change_lines(json_string(block, q0));
        }
    }
    return best;
}

}  // namespace shunti
