#pragma once
#include <string>

struct AppConfig {
    std::string download_path = "sdmc:/pico-8/carts/";
};

extern AppConfig g_config;
void load_config();
void save_config();
