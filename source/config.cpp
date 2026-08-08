#include "config.hpp"
#include <fstream>
#include <sys/stat.h>

AppConfig g_config;

void load_config() {
    std::ifstream file("sdmc:/config/pico8_downloader/settings.txt");
    if (file.is_open()) {
        std::getline(file, g_config.download_path);
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
