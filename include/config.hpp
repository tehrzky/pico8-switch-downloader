#pragma once
#include <string>

struct AppConfig {
    std::string download_path = "sdmc:/pico-8/carts/";
};

extern AppConfig g_config;
void load_config();
void save_config();

// Creates every directory in g_config.download_path if it doesn't already exist,
// e.g. "sdmc:/pico-8/carts/" -> mkdir sdmc:/pico-8, mkdir sdmc:/pico-8/carts.
void ensure_download_dir();
