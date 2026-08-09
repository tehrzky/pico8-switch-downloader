#pragma once
#include <string>
#include <atomic>

// Downloads url -> save_path. If progress_out/bytes_out are non-null, they are
// updated as the transfer proceeds, so this is safe (and intended) to call from
// a background std::thread while the main thread keeps rendering.
bool download_file(const std::string& url,
                    const std::string& save_path,
                    std::atomic<float>* progress_out = nullptr,
                    std::atomic<long>* bytes_out = nullptr);
