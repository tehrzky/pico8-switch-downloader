#include "scraper.hpp"
#include <curl/curl.h>
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

    size_t tid_count = 0, pos = 0;
    while ((pos = body.find("tid=", pos)) != std::string::npos) { tid_count++; pos += 4; }
    f << "occurrences of \"tid=\": " << tid_count << "\n";
    f << "--- first 800 bytes ---\n";
    f << body.substr(0, 800) << "\n";
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

    // Manual parsing: find all <a href="?tid=..."> ... <div>...<div>Title</div></div>
    size_t pos = 0;
    while ((pos = html.find("<a href=\"?tid=", pos)) != std::string::npos) {
        size_t tid_start = pos + 14; // after "?tid="
        size_t tid_end = html.find('"', tid_start);
        if (tid_end == std::string::npos) break;
        std::string tid = html.substr(tid_start, tid_end - tid_start);

        // Find the first <div> after this, then the nested <div> with title
        size_t div1 = html.find("<div", tid_end);
        if (div1 == std::string::npos) break;
        size_t div2 = html.find("<div", div1 + 1);
        if (div2 == std::string::npos) break;
        size_t title_start = html.find('>', div2) + 1;
        size_t title_end = html.find("</div>", title_start);
        if (title_end == std::string::npos) break;
        std::string title = html.substr(title_start, title_end - title_start);

        // Avoid duplicates
        bool dup = false;
        for (auto& existing : out_items) if (existing.tid == tid) { dup = true; break; }
        if (!dup) {
            CartItem item;
            item.tid = tid;
            item.title = decode_entities(title);
            item.thread_url = "https://www.lexaloffle.com/bbs/?tid=" + tid + "&cat=7";
            out_items.push_back(item);
        }
        pos = tid_end;
    }

    if (out_items.empty()) {
        size_t tid_count = 0, p = 0;
        while ((p = html.find("tid=", p)) != std::string::npos) { tid_count++; p += 4; }
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

    // --- Extract author ---
    size_t pos = html.find("?uid=");
    if (pos != std::string::npos) {
        size_t start = html.rfind('>', pos);
        if (start != std::string::npos) {
            start = html.find('>', start) + 1;
            size_t end = html.find("</a>", start);
            if (end != std::string::npos) {
                item.author = decode_entities(html.substr(start, end - start));
            }
        }
    }

    // --- Extract download URL (.p8.png) ---
    const char* markers[] = { "title=\"Open Cartridge File\"", "title='Open Cartridge File'" };
    for (const char* marker : markers) {
        pos = html.find(marker);
        if (pos != std::string::npos) {
            size_t href_pos = html.rfind("href=\"", pos);
            if (href_pos == std::string::npos) href_pos = html.rfind("href='", pos);
            if (href_pos != std::string::npos) {
                size_t start = href_pos + 6;
                size_t end = html.find('"', start);
                if (end == std::string::npos) end = html.find('\'', start);
                if (end != std::string::npos) {
                    std::string url = html.substr(start, end - start);
                    if (url.find(".p8.png") != std::string::npos) {
                        item.download_url = absolute_url(url);
                        break;
                    }
                }
            }
        }
    }
    if (item.download_url.empty()) {
        pos = html.find(".p8.png");
        if (pos != std::string::npos) {
            size_t href_pos = html.rfind("href=\"", pos);
            if (href_pos != std::string::npos) {
                size_t start = href_pos + 6;
                size_t end = html.find('"', start);
                if (end != std::string::npos && end > pos) {
                    item.download_url = absolute_url(html.substr(start, end - start));
                }
            }
        }
    }

    // --- Extract thumbnail (improved) ---
    // First, try to find any <img> tag with src containing "/bbs/thumbs/"
    pos = 0;
    while ((pos = html.find("<img", pos)) != std::string::npos) {
        size_t src_pos = html.find("src=", pos);
        if (src_pos == std::string::npos) break;
        
        size_t start = src_pos + 5; // after "src="
        size_t end = html.find('"', start);
        if (end == std::string::npos) end = html.find('\'', start);
        if (end == std::string::npos) { pos++; continue; }
        
        std::string src = html.substr(start, end - start);
        // Check if this looks like a thumbnail
        if (src.find("/bbs/thumbs/") != std::string::npos) {
            item.thumbnail_url = absolute_url(src);
            break;
        }
        pos = end;
    }

    // Fallback: any image that is not gfx/icons/avatar
    if (item.thumbnail_url.empty()) {
        pos = 0;
        while ((pos = html.find("<img", pos)) != std::string::npos) {
            size_t src_pos = html.find("src=", pos);
            if (src_pos == std::string::npos) break;
            
            size_t start = src_pos + 5;
            size_t end = html.find('"', start);
            if (end == std::string::npos) end = html.find('\'', start);
            if (end == std::string::npos) { pos++; continue; }
            
            std::string src = html.substr(start, end - start);
            if (src.find("/gfx/") == std::string::npos &&
                src.find("/icons/") == std::string::npos &&
                src.find("avatar") == std::string::npos &&
                (src.find(".png") != std::string::npos || src.find(".gif") != std::string::npos)) {
                item.thumbnail_url = absolute_url(src);
                break;
            }
            pos = end;
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
