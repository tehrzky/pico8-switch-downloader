#include "downloader.hpp"
#include <curl/curl.h>
#include <fstream>
#include <cstdio>

namespace {

struct ProgressCtx {
    std::atomic<float>* progress;
    std::atomic<long>* bytes;
};

size_t write_callback(char* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* out_file = static_cast<std::ofstream*>(userp);
    out_file->write(contents, total_size);
    return out_file->good() ? total_size : 0;
}

int xfer_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<ProgressCtx*>(clientp);
    if (ctx->bytes) ctx->bytes->store((long)dlnow);
    if (ctx->progress) {
        ctx->progress->store(dltotal > 0 ? (float)dlnow / (float)dltotal : 0.0f);
    }
    return 0; // returning non-zero would abort the transfer
}

} // namespace

bool download_file(const std::string& url, const std::string& save_path,
                    std::atomic<float>* progress_out, std::atomic<long>* bytes_out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::ofstream out_file(save_path, std::ios::binary);
    if (!out_file.is_open()) {
        curl_easy_cleanup(curl);
        return false;
    }

    ProgressCtx ctx{ progress_out, bytes_out };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_file);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Pico8SwitchDownloader/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    out_file.close();

    bool ok = (res == CURLE_OK) && (http_code == 200);
    if (!ok) std::remove(save_path.c_str());
    if (progress_out) progress_out->store(ok ? 1.0f : progress_out->load());
    return ok;
}
