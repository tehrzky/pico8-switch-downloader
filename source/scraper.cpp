#include "scraper.hpp"
#include <curl/curl.h>
#include <regex>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace {

size_t string_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t bytes_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<unsigned char>*>(userdata);
    size_t total = size * nmemb;
    out->insert(out->end(), ptr, ptr + total);
    return total;
}

bool http_get_text(const std::string& url, std::string& out, long& http_code, std::string& curl_err) {
    http_code = 0;
    curl_err.clear();
    CURL* curl = curl_easy_init();
    if (!curl) { curl_err = "curl_easy_init failed"; return false; }
    out.clear();

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, string_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                      "Mozilla/5.0 (Nintendo Switch) Pico8SwitchDownloader/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        curl_err = std::string(curl_easy_strerror(res));
        if (errbuf[0]) curl_err += " (" + std::string(errbuf) + ")";
    }

    return (res == CURLE_OK) && (http_code == 200);
}

std::string decode_entities(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ) {
        if (in.compare(i, 5, "&amp;") == 0) { out += '&'; i += 5; }
        else if (in.compare(i, 6, "&#039;") == 0) { out += '\''; i += 6; }
        else if (in.compare(i, 5, "&#39;") == 0) { out += '\''; i += 5; }
        else if (in.compare(i, 6, "&quot;") == 0) { out += '"'; i += 6; }
        else if (in.compare(i, 4, "&lt;") == 0) { out += '<'; i += 4; }
        else if (in.compare(i, 4, "&gt;") == 0) { out += '>'; i += 4; }
        else { out += in[i]; i += 1; }
    }
    return out;
}

std::string url_encode(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else if (c == ' ') {
            out += '+';
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string filter_query(CartFilter f) {
    switch (f) {
        case CartFilter::New:      return "orderby=new";
        case CartFilter::Popular:  return "orderby=featured&popular=1";
        case CartFilter::TopRated: return "orderby=favourite";
        case CartFilter::Featured:
        default:                   return "orderby=featured";
    }
}

std::string absolute_url(const std::string& href) {
    if (href.empty()) return href;
    if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) return href;
    if (href[0] == '/') return "https://www.lexaloffle.com" + href;
    return "https://www.lexaloffle.com/bbs/" + href;
}

void dump_debug(const std::string& label, const std::string& url, long http_code,
                 const std::string& curl_err, const std::string& body) {
    std::ofstream f("sdmc:/switch/pico8-downloader/debug_last_fetch.txt", std::ios::trunc);
    if (!f.is_open()) return;
    f << "=== " << label << " ===\n";
    f << "URL: " << url << "\n";
    f << "HTTP code: " << http_code << "\n";
    f << "curl error: " << (curl_err.empty() ? "(none)" : curl_err) << "\n";
    f << "Body length: " << body.size() << " bytes\n";

    size_t shown = 0, pos = 0;
    f << "--- context around each \"tid=\" occurrence (first 6) ---\n";
    while ((pos = body.find("tid=", pos)) != std::string::npos && shown < 6) {
        size_t start = (pos > 150) ? pos - 150 : 0;
        size_t len = std::min<size_t>(300, body.size() - start);
        f << "\n[occurrence " << (shown + 1) << " at byte " << pos << "]\n";
        f << body.substr(start, len) << "\n";
        pos += 4;
        shown++;
    }

    f << "\n--- first 2000 bytes of body (for reference) ---\n";
    f << body.substr(0, 2000) << "\n";
    f.close();
}

} // namespace

bool fetch_cart_list(CartFilter filter,
                      const std::string& search,
                      int page,
                      std::vector<CartItem>& out_items,
                      std::string& out_error) {
    out_items.clear();
    out_error.clear();

    std::string url = "https://www.lexaloffle.com/bbs/?cat=7&sub=2&mode=carts&page=" +
                       std::to_string(page) + "&" + filter_query(filter);
    if (!search.empty()) {
        url += "&keywords=" + url_encode(search);
    }

    std::string html;
    long http_code = 0;
    std::string curl_err;
    bool ok = http_get_text(url, html, http_code, curl_err);
    dump_debug("fetch_cart_list", url, http_code, curl_err, html);

    if (!ok) {
        out_error = "Fetch failed: HTTP " + std::to_string(http_code) +
                    (curl_err.empty() ? "" : (" - " + curl_err));
        return false;
    }

    static const std::regex item_re(R"(<a[^>]+href=["'][^"']*[?&]tid=(\d+)["'][^>]*>([^<]+)</a>)");
    auto begin = std::sregex_iterator(html.begin(), html.end(), item_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string tid = (*it)[1].str();
        bool dup = false;
        for (auto& existing : out_items) if (existing.tid == tid) { dup = true; break; }
        if (dup) continue;

        CartItem item;
        item.tid = tid;
        item.title = decode_entities((*it)[2].str());
        item.thread_url = "https://www.lexaloffle.com/bbs/?tid=" + tid + "&cat=7";
        out_items.push_back(item);
    }

    if (out_items.empty()) {
        size_t tid_count = 0, pos = 0;
        while ((pos = html.find("tid=", pos)) != std::string::npos) { tid_count++; pos += 4; }
        out_error = "Parsed 0 carts from a " + std::to_string(html.size()) +
                    "-byte response (found \"tid=\" " + std::to_string(tid_count) +
                    " times) - check sdmc:/switch/pico8-downloader/debug_last_fetch.txt";
    }
    return true;
}

void resolve_cart_detail(CartItem& item) {
    item.detail_resolved = true;
    if (item.tid.empty()) return;

    std::string html;
    long http_code = 0;
    std::string curl_err;
    if (!http_get_text(item.thread_url, html, http_code, curl_err)) return;

    static const std::regex author_re(R"(<a[^>]+href=["'][^"']*[?&]uid=\d+["'][^>]*>([^<]+)</a>)");
    std::smatch m;
    if (std::regex_search(html, m, author_re)) {
        item.author = decode_entities(m[1].str());
    }

    static const std::regex cart_re1(R"(<a[^>]+href=["']([^"']+\.p8\.png)["'][^>]*title=["']Open Cartridge File["'])");
    static const std::regex cart_re2(R"(<a[^>]+title=["']Open Cartridge File["'][^>]*href=["']([^"']+\.p8\.png)["'])");
    static const std::regex cart_re_fallback(R"(["']([^"'\s>]+\.p8\.png)["'])");
    if (std::regex_search(html, m, cart_re1) || std::regex_search(html, m, cart_re2) ||
        std::regex_search(html, m, cart_re_fallback)) {
        item.download_url = absolute_url(m[1].str());
    }

    static const std::regex thumb_re(R"(<img[^>]+src=["']([^"']*\/bbs\/thumbs\/[^"']+\.(?:png|gif))["'])");
    if (std::regex_search(html, m, thumb_re)) {
        item.thumbnail_url = absolute_url(m[1].str());
    } else {
        std::string head = html.substr(0, std::min<size_t>(html.size(), 6000));
        static const std::regex loose_img_re(R"(<img[^>]+src=["']([^"']+\.(?:png|gif))["'])");
        auto begin = std::sregex_iterator(head.begin(), head.end(), loose_img_re);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string candidate = (*it)[1].str();
            if (candidate.find("/gfx/") != std::string::npos) continue;
            if (candidate.find("/icons/") != std::string::npos) continue;
            if (candidate.find("avatar") != std::string::npos) continue;
            item.thumbnail_url = absolute_url(candidate);
            break;
        }
    }
}

bool http_get_binary(const std::string& url, std::vector<unsigned char>& out) {
    if (url.empty()) return false;
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    out.clear();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bytes_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Pico8SwitchDownloader/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) && (http_code == 200) && !out.empty();
}
