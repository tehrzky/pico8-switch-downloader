#include "scraper.hpp"
#include <curl/curl.h>
#include <regex>
#include <cctype>
#include <cstdio>

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

bool http_get_text(const std::string& url, std::string& out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    out.clear();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, string_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                      "Mozilla/5.0 (Nintendo Switch) Pico8SwitchDownloader/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

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
        case CartFilter::New:      return "orderby=ts";
        case CartFilter::Popular:  return "orderby=ts&popular=1";
        case CartFilter::TopRated: return "orderby=favourite";
        case CartFilter::Featured:
        default:                   return "orderby=lucky";
    }
}

} // namespace

bool fetch_cart_list(CartFilter filter,
                      const std::string& search,
                      int page,
                      std::vector<CartItem>& out_items,
                      std::string& out_error) {
    out_items.clear();
    out_error.clear();

    std::string url = "https://www.lexaloffle.com/bbs/lister.php?cat=7&mode=carts&sub=2&page=" +
                       std::to_string(page) + "&" + filter_query(filter);
    if (!search.empty()) {
        url += "&keywords=" + url_encode(search);
    }

    std::string html;
    if (!http_get_text(url, html)) {
        out_error = "Couldn't reach lexaloffle.com. Check your Switch's internet connection.";
        return false;
    }

    // Cart entries are anchor tags linking to a thread id, e.g.:
    //   <a href="/bbs/?tid=46754" ...>Still a Magical Girl</a>
    static const std::regex item_re(R"(<a[^>]+href=\"[^\"]*[?&]tid=(\d+)\"[^>]*>([^<]+)</a>)");
    auto begin = std::sregex_iterator(html.begin(), html.end(), item_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string tid = (*it)[1].str();

        bool dup = false;
        for (auto& existing : out_items) {
            if (existing.tid == tid) { dup = true; break; }
        }
        if (dup) continue;

        CartItem item;
        item.tid = tid;
        item.title = decode_entities((*it)[2].str());
        item.thread_url = "https://www.lexaloffle.com/bbs/?tid=" + tid + "&cat=7";
        out_items.push_back(item);
    }

    if (out_items.empty()) {
        out_error = "No carts found for this filter/search.";
    }
    return true;
}

void resolve_cart_detail(CartItem& item) {
    item.detail_resolved = true;
    if (item.tid.empty()) return;

    std::string html;
    if (!http_get_text(item.thread_url, html)) return;

    // Author: "...?uid=123"><b>Name</b>" style link near the top of the thread.
    static const std::regex author_re(R"(\?uid=\d+\"[^>]*>\s*<b>([^<]+)</b>)");
    std::smatch m;
    if (std::regex_search(html, m, author_re)) {
        item.author = decode_entities(m[1].str());
    }

    // The cartridge file itself is always named "*.p8.png" wherever it's linked from.
    static const std::regex cart_re(R"((https?://[^\"'\s>]+\.p8\.png))");
    if (std::regex_search(html, m, cart_re)) {
        item.download_url = m[1].str();
    }

    // Thumbnail preview image, if we can find one.
    static const std::regex thumb_re(R"((https?://[^\"'\s>]*/bbs/thums?/[^\"'\s>]+\.(?:png|gif)))");
    if (std::regex_search(html, m, thumb_re)) {
        item.thumbnail_url = m[1].str();
    } else {
        // Fallback: the strict "thumbs" path didn't match - try any image reference
        // near the top of the page that isn't obvious site chrome (nav icons, logos,
        // avatars). This is a looser heuristic in case the site's actual thumbnail
        // path differs from what we assumed.
        std::string head = html.substr(0, std::min<size_t>(html.size(), 6000));
        static const std::regex loose_img_re(R"(<img[^>]+src=\"(https?://[^\"]+\.(?:png|gif))\")");
        auto begin = std::sregex_iterator(head.begin(), head.end(), loose_img_re);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string candidate = (*it)[1].str();
            if (candidate.find("/gfx/") != std::string::npos) continue;
            if (candidate.find("/icons/") != std::string::npos) continue;
            if (candidate.find("avatar") != std::string::npos) continue;
            item.thumbnail_url = candidate;
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) && (http_code == 200) && !out.empty();
}
