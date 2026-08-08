#include "downloader.hpp"
#include <curl/curl.h>
#include <fstream>

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* out_file = static_cast<std::ofstream*>(userp);
    out_file->write(static_cast<char*>(contents), total_size);
    return total_size;
}

bool download_file(const std::string& url, const std::string& save_path) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::ofstream out_file(save_path, std::ios::binary);
    if (!out_file.is_open()) {
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_file);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Pico8SwitchDownloader/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    out_file.close();

    return (res == CURLE_OK);
}
