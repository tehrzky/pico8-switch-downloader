#include "config.hpp"
#include <fstream>
#include <sys/stat.h>

AppConfig g_config;

void load_config() {
    std::ifstream file("sdmc:/config/pico8_downloader/settings.txt");
    if (file.is_open()) {
        std::string line;
        if (std::getline(file, line) && !line.empty()) {
            g_config.download_path = line;
        }
        file.close();
    }
}

void save_config() {
    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/pico8_downloader", 0777);
    std::ofstream file("sdmc:/config/pico8_downloader/settings.txt");
    if (file.is_open()) {
        file << g_config.download_path << "\n";
        file.close();
    }
}

void ensure_download_dir() {
    // Build up the path one directory at a time, e.g.
    // "sdmc:/pico-8/carts/" -> "sdmc:/pico-8", "sdmc:/pico-8/carts"
    std::string path = g_config.download_path;
    std::string partial;
    size_t i = 0;
    while (i < path.size()) {
        size_t slash = path.find('/', i);
        if (slash == std::string::npos) slash = path.size();
        partial = path.substr(0, slash);
        if (partial.size() > 6) { // longer than "sdmc:/"
            mkdir(partial.c_str(), 0777);
        }
        i = slash + 1;
    }
}
