#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include "config.hpp"
#include "downloader.hpp"
#include "scraper.hpp"

// Screen Resolution
constexpr int SCREEN_WIDTH = 1280;
constexpr int SCREEN_HEIGHT = 720;

// Layout constants
constexpr int SIDEBAR_X = 20;
constexpr int SIDEBAR_W = 360;
constexpr int RESULTS_X = 400;
constexpr int RESULTS_W = 860;
constexpr int RESULTS_Y = 60;
constexpr int RESULTS_H = 610;

// Colors (RGBA)
const SDL_Color COLOR_BG       = { 20,  20,  20, 255 };
const SDL_Color COLOR_PANEL    = { 32,  32,  32, 255 };
const SDL_Color COLOR_BORDER   = { 60,  60,  60, 255 };
const SDL_Color COLOR_ACCENT   = {  0, 180, 120, 255 };
const SDL_Color COLOR_WHITE    = { 255, 255, 255, 255 };
const SDL_Color COLOR_MUTED    = { 160, 160, 160, 255 };
const SDL_Color COLOR_SELECTED = { 45,  90, 140, 255 };
const SDL_Color COLOR_ERROR    = { 200, 70,  70, 255 };
const SDL_Color COLOR_LOADING  = { 0,  180, 120, 255 };

enum class Focus { SEARCH, FILTERS, PATH, RESULTS };

// --- Globals shared with background threads ---
static std::atomic<bool>  g_download_active{false};
static std::atomic<float> g_download_progress{0.0f};
static std::atomic<long>  g_download_bytes{0};
static std::atomic<bool>  g_download_success{false};
static std::thread        g_download_thread;
static std::string        g_download_label;
static int                g_downloading_index = -1;

// --- Background cart-list fetch ---
static std::atomic<bool>  g_list_fetch_active{false};
static std::atomic<bool>  g_list_fetch_done{false};
static std::thread        g_list_fetch_thread;
static std::vector<CartItem> g_fetched_carts;
static std::string        g_fetch_error;
static bool               g_fetch_error_flag = false;

// --- Background detail worker result queue ---
// The worker NEVER touches the main carts vector. It works on a copy
// and pushes results here. Main thread applies them safely.
struct DetailResult {
    std::string tid;
    std::string author;
    std::string download_url;
    std::string thumbnail_url;
    std::vector<unsigned char> thumb_bytes;
};
static std::atomic<bool>  g_detail_worker_active{false};
static std::thread        g_detail_worker_thread;
static std::mutex         g_detail_queue_mtx;
static std::vector<DetailResult> g_detail_queue;

// --- Text texture cache ---
struct TextCache {
    std::unordered_map<std::string, SDL_Texture*> map;

    SDL_Texture* get(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, SDL_Color color) {
        if (text.empty()) return nullptr;
        char key[24];
        snprintf(key, sizeof(key), "%p|%02x%02x%02x|", (void*)font, color.r, color.g, color.b);
        std::string full_key = std::string(key) + text;
        auto it = map.find(full_key);
        if (it != map.end()) return it->second;

        SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!surf) return nullptr;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
        SDL_FreeSurface(surf);
        map[full_key] = tex;
        return tex;
    }

    void clear() {
        for (auto& kv : map) SDL_DestroyTexture(kv.second);
        map.clear();
    }
};

static void draw_rect(SDL_Renderer* renderer, int x, int y, int w, int h, SDL_Color color, bool fill = true) {
    SDL_Rect rect = { x, y, w, h };
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    if (fill) SDL_RenderFillRect(renderer, &rect);
    else SDL_RenderDrawRect(renderer, &rect);
}

static int draw_text(SDL_Renderer* renderer, TextCache& cache, TTF_Font* font,
                      const std::string& text, int x, int y, SDL_Color color,
                      int max_width = -1) {
    if (!font || text.empty()) return 0;
    std::string shown = text;
    if (max_width > 0) {
        int w = 0, h = 0;
        TTF_SizeUTF8(font, shown.c_str(), &w, &h);
        while (w > max_width && shown.size() > 1) {
            shown.pop_back();
            TTF_SizeUTF8(font, (shown + "...").c_str(), &w, &h);
        }
        if (shown.size() < text.size()) shown += "...";
    }
    SDL_Texture* tex = cache.get(renderer, font, shown, color);
    if (!tex) return 0;
    int w, h;
    SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
    SDL_Rect dst = { x, y, w, h };
    SDL_RenderCopy(renderer, tex, nullptr, &dst);
    return w;
}

static std::string sanitize_filename(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_') out += c;
    }
    if (out.empty()) out = "cart";
    return out;
}

static std::string filter_label(CartFilter f) {
    switch (f) {
        case CartFilter::New:      return "NEW";
        case CartFilter::Popular:  return "POPULAR";
        case CartFilter::TopRated: return "TOP RATED";
        case CartFilter::Featured:
        default:                   return "FEATURED";
    }
}

static std::string prompt_keyboard(const std::string& guide, const std::string& initial) {
    SwkbdConfig kbd;
    std::string result = initial;
    if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetGuideText(&kbd, guide.c_str());
        swkbdConfigSetInitialText(&kbd, initial.c_str());
        char out_buf[256] = {0};
        if (R_SUCCEEDED(swkbdShow(&kbd, out_buf, sizeof(out_buf)))) {
            result = out_buf;
        }
        swkbdClose(&kbd);
    }
    return result;
}

// ------------------------------------------------------------------
// Background worker: resolves details on a COPY, pushes to queue
// ------------------------------------------------------------------
static void detail_worker_loop(std::vector<CartItem> carts_copy) {
    for (auto& c : carts_copy) {
        if (!g_detail_worker_active.load()) break;
        if (c.detail_resolved) continue;

        resolve_cart_detail(c);

        DetailResult res;
        res.tid = c.tid;
        res.author = c.author;
        res.download_url = c.download_url;
        res.thumbnail_url = c.thumbnail_url;

        if (!c.thumbnail_url.empty()) {
            http_get_binary(c.thumbnail_url, res.thumb_bytes);
        }

        std::lock_guard<std::mutex> lk(g_detail_queue_mtx);
        g_detail_queue.push_back(std::move(res));
    }
    g_detail_worker_active.store(false);
}

static void stop_detail_worker() {
    g_detail_worker_active.store(false);
    if (g_detail_worker_thread.joinable()) g_detail_worker_thread.join();
}

static void start_detail_worker(const std::vector<CartItem>& carts) {
    stop_detail_worker();
    // Clear stale results from previous page
    {
        std::lock_guard<std::mutex> lk(g_detail_queue_mtx);
        g_detail_queue.clear();
    }
    g_detail_worker_active.store(true);
    // Pass by value (copy) so the thread never touches main carts vector
    g_detail_worker_thread = std::thread(detail_worker_loop, carts);
}

// ------------------------------------------------------------------
// Launch a non-blocking cart-list fetch
// ------------------------------------------------------------------
static void launch_list_fetch(CartFilter filter, const std::string& search, int page) {
    if (g_list_fetch_active.load()) return;

    g_list_fetch_active.store(true);
    g_list_fetch_done.store(false);
    g_fetch_error.clear();
    g_fetch_error_flag = false;
    g_fetched_carts.clear();

    if (g_list_fetch_thread.joinable()) g_list_fetch_thread.join();

    g_list_fetch_thread = std::thread([filter, search, page]() {
        std::string err;
        bool ok = fetch_cart_list(filter, search, page, g_fetched_carts, err);
        if (!ok) {
            g_fetch_error = err;
            g_fetch_error_flag = true;
        } else if (!err.empty()) {
            g_fetch_error = err;
            g_fetch_error_flag = false;
        }
        g_list_fetch_done.store(true);
        g_list_fetch_active.store(false);
    });
}

// ------------------------------------------------------------------
// Draw a loading spinner inside the results panel only
// ------------------------------------------------------------------
static void draw_loading_overlay(SDL_Renderer* renderer, TextCache& cache, TTF_Font* font, int frame) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    draw_rect(renderer, RESULTS_X, RESULTS_Y, RESULTS_W, RESULTS_H, {0, 0, 0, 160});
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    const char* dots[] = { ".  ", ".. ", "...", "   " };
    std::string msg = "Loading carts" + std::string(dots[(frame / 15) % 4]);
    int tw = 0, th = 0;
    if (font) TTF_SizeUTF8(font, msg.c_str(), &tw, &th);
    int cx = RESULTS_X + RESULTS_W / 2 - tw / 2;
    int cy = RESULTS_Y + RESULTS_H / 2 - th / 2;
    draw_text(renderer, cache, font, msg, cx, cy, COLOR_LOADING);
}

int main(int argc, char **argv) {
    socketInitializeDefault();
    romfsInit();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    IMG_Init(IMG_INIT_PNG);
    TTF_Init();

    SDL_Window* window = SDL_CreateWindow("PICO-8 Downloader",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          SCREEN_WIDTH, SCREEN_HEIGHT, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    const char* regular_candidates[] = {
        "sdmc:/switch/pico8-downloader/PTSans-Regular.ttf",
        "romfs:/PTSans-Regular.ttf",
    };
    const char* bold_candidates[] = {
        "sdmc:/switch/pico8-downloader/PTSans-Bold.ttf",
        "romfs:/PTSans-Bold.ttf",
    };
    std::string font_regular_path, font_bold_path;
    TTF_Font* font_header = nullptr;
    TTF_Font* font_bold   = nullptr;
    TTF_Font* font_body   = nullptr;
    TTF_Font* font_small  = nullptr;
    for (const char* p : bold_candidates) {
        font_header = TTF_OpenFont(p, 24);
        if (font_header) { font_bold_path = p; break; }
    }
    if (!font_bold_path.empty()) font_bold = TTF_OpenFont(font_bold_path.c_str(), 20);
    for (const char* p : regular_candidates) {
        font_body = TTF_OpenFont(p, 18);
        if (font_body) { font_regular_path = p; break; }
    }
    if (!font_regular_path.empty()) font_small = TTF_OpenFont(font_regular_path.c_str(), 15);

    bool fonts_ok = (font_header && font_bold && font_body && font_small);
    std::string font_source = font_regular_path.empty() ? "none" :
        (font_regular_path.find("romfs:") == 0 ? "romfs (auto)" : "sdmc (manual copy)");
    TextCache text_cache;

    load_config();
    ensure_download_dir();

    std::vector<CartItem> carts;
    std::unordered_map<std::string, SDL_Texture*> thumb_textures;

    CartFilter current_filter = CartFilter::Popular;
    std::string search_text;
    int current_page = 1;
    int selected_filter = 2;
    int selected_cart = 0;
    int scroll_offset = 0;
    Focus focus = Focus::FILTERS;
    std::string status_message = "Press Y to load carts.";
    bool status_is_error = false;
    bool has_loaded_once = false;
    std::string thumb_fetch_status;
    std::string debug_line;

    const char* filters_label[] = { "FEATURED", "NEW", "POPULAR", "TOP RATED" };
    const CartFilter filters_value[] = { CartFilter::Featured, CartFilter::New, CartFilter::Popular, CartFilter::TopRated };

    auto apply_fetched_list = [&](bool reset_page) {
        if (reset_page) current_page = 1;
        text_cache.clear();
        selected_cart = 0;
        scroll_offset = 0;

        if (g_fetch_error_flag) {
            status_message = g_fetch_error;
            status_is_error = true;
            carts.clear();
        } else if (!g_fetch_error.empty()) {
            status_message = g_fetch_error;
            status_is_error = false;
            carts.clear();
        } else {
            carts = std::move(g_fetched_carts);
            status_message = std::to_string(carts.size()) + " carts found.";
            status_is_error = false;
            if (!carts.empty()) start_detail_worker(carts);
        }
        has_loaded_once = true;
    };

    auto refresh_list = [&](bool reset_page) {
        if (reset_page) {
            current_page = 1;
            for (auto& kv : thumb_textures) SDL_DestroyTexture(kv.second);
            thumb_textures.clear();
        }
        selected_cart = 0;
        scroll_offset = 0;
        stop_detail_worker();
        launch_list_fetch(current_filter, search_text, current_page);
    };

    int frame_counter = 0;
    bool running = true;
    while (appletMainLoop() && running) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) running = false;

        // --- Background fetch completed? ---
        if (g_list_fetch_done.load()) {
            g_list_fetch_done.store(false);
            apply_fetched_list(false);
        }

        // --- Apply detail results from background worker (main thread only) ---
        {
            std::lock_guard<std::mutex> lk(g_detail_queue_mtx);
            for (auto& res : g_detail_queue) {
                // Find matching cart in main vector
                for (auto& c : carts) {
                    if (c.tid != res.tid) continue;
                    c.author = std::move(res.author);
                    c.download_url = std::move(res.download_url);
                    c.thumbnail_url = std::move(res.thumbnail_url);
                    c.detail_resolved = true;
                    break;
                }
                // Create SDL texture from thumb bytes (main thread only)
                if (!res.thumb_bytes.empty() && thumb_textures.find(res.tid) == thumb_textures.end()) {
                    SDL_RWops* rw = SDL_RWFromMem(res.thumb_bytes.data(), (int)res.thumb_bytes.size());
                    SDL_Surface* surf = IMG_Load_RW(rw, 1);
                    if (surf) {
                        thumb_textures[res.tid] = SDL_CreateTextureFromSurface(renderer, surf);
                        SDL_FreeSurface(surf);
                    }
                }
            }
            g_detail_queue.clear();
        }

        bool is_loading = g_list_fetch_active.load();

        if (!is_loading) {
            if (kDown & HidNpadButton_Down) {
                if (focus == Focus::SEARCH) focus = Focus::FILTERS;
                else if (focus == Focus::FILTERS) {
                    if (selected_filter < 3) selected_filter++;
                    else focus = Focus::PATH;
                } else if (focus == Focus::RESULTS && !carts.empty()) {
                    if ((size_t)(selected_cart + 2) < carts.size()) {
                        selected_cart += 2;
                        if (selected_cart >= scroll_offset + 4) scroll_offset += 2;
                    }
                }
            }
            if (kDown & HidNpadButton_Up) {
                if (focus == Focus::FILTERS) {
                    if (selected_filter > 0) selected_filter--;
                    else focus = Focus::SEARCH;
                } else if (focus == Focus::PATH) {
                    focus = Focus::FILTERS;
                } else if (focus == Focus::RESULTS && !carts.empty()) {
                    if (selected_cart - 2 >= 0) {
                        selected_cart -= 2;
                        if (selected_cart < scroll_offset) scroll_offset -= 2;
                        if (scroll_offset < 0) scroll_offset = 0;
                    }
                }
            }
            if (kDown & HidNpadButton_Right) {
                if (focus == Focus::SEARCH || focus == Focus::FILTERS || focus == Focus::PATH) {
                    focus = Focus::RESULTS;
                } else if (focus == Focus::RESULTS && !carts.empty()) {
                    if ((selected_cart % 2 == 0) && (size_t)(selected_cart + 1) < carts.size()) {
                        selected_cart++;
                        if (selected_cart >= scroll_offset + 4) scroll_offset += 2;
                    }
                }
            }
            if (kDown & HidNpadButton_Left) {
                if (focus == Focus::RESULTS) {
                    if (selected_cart % 2 == 1) {
                        selected_cart--;
                        if (selected_cart < scroll_offset) scroll_offset -= 2;
                        if (scroll_offset < 0) scroll_offset = 0;
                    } else {
                        focus = Focus::FILTERS;
                    }
                }
            }

            if (kDown & HidNpadButton_B) {
                if (focus == Focus::RESULTS) focus = Focus::FILTERS;
            }

            if (kDown & HidNpadButton_Y) {
                refresh_list(true);
            }

            if (kDown & HidNpadButton_L) {
                if (current_page > 1) { current_page--; refresh_list(false); }
            }
            if (kDown & HidNpadButton_R) {
                current_page++;
                refresh_list(false);
            }

            if (kDown & HidNpadButton_X) {
                std::string new_path = prompt_keyboard("Download folder (e.g. sdmc:/pico-8/carts/)", g_config.download_path);
                if (!new_path.empty()) {
                    if (new_path.back() != '/') new_path += '/';
                    g_config.download_path = new_path;
                    save_config();
                    ensure_download_dir();
                }
            }

            if (kDown & HidNpadButton_A) {
                if (focus == Focus::SEARCH) {
                    search_text = prompt_keyboard("Search Lexaloffle BBS...", search_text);
                    refresh_list(true);
                } else if (focus == Focus::FILTERS) {
                    current_filter = filters_value[selected_filter];
                    refresh_list(true);
                } else if (focus == Focus::PATH) {
                    std::string new_path = prompt_keyboard("Download folder", g_config.download_path);
                    if (!new_path.empty()) {
                        if (new_path.back() != '/') new_path += '/';
                        g_config.download_path = new_path;
                        save_config();
                        ensure_download_dir();
                    }
                } else if (focus == Focus::RESULTS && !carts.empty() && (size_t)selected_cart < carts.size()) {
                    CartItem& item = carts[selected_cart];
                    if (!item.detail_resolved) {
                        status_message = "Looking up download link...";
                        status_is_error = false;
                        resolve_cart_detail(item);
                        // On-demand thumb fetch for immediate feedback
                        if (!item.thumbnail_url.empty() && thumb_textures.find(item.tid) == thumb_textures.end()) {
                            std::vector<unsigned char> bytes;
                            if (http_get_binary(item.thumbnail_url, bytes)) {
                                SDL_RWops* rw = SDL_RWFromMem(bytes.data(), (int)bytes.size());
                                SDL_Surface* surf = IMG_Load_RW(rw, 1);
                                if (surf) {
                                    thumb_textures[item.tid] = SDL_CreateTextureFromSurface(renderer, surf);
                                    SDL_FreeSurface(surf);
                                }
                            }
                        }
                    }
                    if (item.download_url.empty()) {
                        status_message = "Couldn't find a direct download link for \"" + item.title +
                                          "\" - try it from lexaloffle.com instead.";
                        status_is_error = true;
                    } else if (!g_download_active.load()) {
                        ensure_download_dir();
                        std::string dest = g_config.download_path + sanitize_filename(item.title) + ".p8.png";
                        std::string url = item.download_url;
                        g_download_progress = 0.0f;
                        g_download_bytes = 0;
                        g_download_active = true;
                        g_download_label = item.title;
                        g_downloading_index = selected_cart;
                        item.downloading = true;
                        if (g_download_thread.joinable()) g_download_thread.join();
                        g_download_thread = std::thread([url, dest]() {
                            bool ok = download_file(url, dest, &g_download_progress, &g_download_bytes);
                            g_download_success = ok;
                            g_download_active = false;
                        });
                    }
                }
            }
        }

        // --- Pick up finished download ---
        if (!g_download_active.load() && g_downloading_index >= 0) {
            if ((size_t)g_downloading_index < carts.size()) {
                carts[g_downloading_index].downloading = false;
                status_message = (g_download_success.load() ? "Downloaded " : "Failed to download ") + g_download_label;
                status_is_error = !g_download_success.load();
            }
            g_downloading_index = -1;
            if (g_download_thread.joinable()) g_download_thread.join();
        }

        // --- Debug line ---
        if (focus == Focus::RESULTS && !carts.empty() && (size_t)selected_cart < carts.size()) {
            CartItem& sel = carts[selected_cart];
            if (sel.detail_resolved) {
                debug_line = "cover: " + (sel.thumbnail_url.empty() ? std::string("not found") : std::string("found")) +
                             "  |  cart: " + (sel.download_url.empty() ? std::string("not found") : std::string("found"));
            }
        }

        // ================================================================
        // RENDER
        // ================================================================
        SDL_SetRenderDrawColor(renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, 255);
        SDL_RenderClear(renderer);

        draw_rect(renderer, 0, 0, SCREEN_WIDTH, 6, fonts_ok ? SDL_Color{0,200,0,255} : SDL_Color{220,0,0,255});

        // Header
        draw_rect(renderer, 0, 0, SCREEN_WIDTH, 45, COLOR_PANEL);
        draw_rect(renderer, 0, 44, SCREEN_WIDTH, 1, COLOR_BORDER);
        draw_text(renderer, text_cache, font_header, "PICO-8 Cart Browser & Downloader v1.1", 20, 8, COLOR_WHITE);
        {
            const char* credit = "by tehrzky";
            int cw = 0, ch = 0;
            if (font_body) TTF_SizeUTF8(font_body, credit, &cw, &ch);
            draw_text(renderer, text_cache, font_body, credit, SCREEN_WIDTH - cw - 20, 13, COLOR_MUTED);
        }

        // Sidebar
        draw_rect(renderer, SIDEBAR_X, 60, SIDEBAR_W, 610, COLOR_PANEL);
        draw_rect(renderer, SIDEBAR_X, 60, SIDEBAR_W, 610, COLOR_BORDER, false);
        draw_text(renderer, text_cache, font_bold, "Filters & Search", 35, 75, COLOR_WHITE);

        {
            SDL_Color box_color = (focus == Focus::SEARCH) ? COLOR_SELECTED : COLOR_BG;
            draw_rect(renderer, 35, 105, 330, 45, box_color);
            draw_rect(renderer, 35, 105, 330, 45, COLOR_BORDER, false);
            std::string shown = search_text.empty() ? "Search Lexaloffle BBS..." : search_text;
            SDL_Color txt_color = search_text.empty() ? COLOR_MUTED : COLOR_WHITE;
            draw_text(renderer, text_cache, font_body, shown, 48, 118, txt_color, 300);
        }

        for (int i = 0; i < 4; i++) {
            bool is_current = (filters_value[i] == current_filter);
            bool is_focused = (focus == Focus::FILTERS && selected_filter == i);
            SDL_Color btn_color = is_focused ? COLOR_SELECTED : (is_current ? SDL_Color{40, 70, 55, 255} : COLOR_BG);
            SDL_Color border_color = is_current ? COLOR_ACCENT : COLOR_BORDER;
            int by = 200 + (i * 55);
            draw_rect(renderer, 35, by, 330, 45, btn_color);
            draw_rect(renderer, 35, by, 330, 45, border_color, false);
            draw_text(renderer, text_cache, font_bold, filters_label[i], 50, by + 12, COLOR_WHITE);
        }

        {
            SDL_Color box_color = (focus == Focus::PATH) ? COLOR_SELECTED : COLOR_BG;
            draw_rect(renderer, 35, 460, 330, 45, box_color);
            draw_rect(renderer, 35, 460, 330, 45, COLOR_BORDER, false);
            draw_text(renderer, text_cache, font_small, "Download path:", 45, 440, COLOR_MUTED);
            draw_text(renderer, text_cache, font_body, g_config.download_path, 48, 473, COLOR_WHITE, 300);
        }

        {
            SDL_Color msg_color = status_is_error ? COLOR_ERROR : COLOR_MUTED;
            draw_text(renderer, text_cache, font_small, status_message, 35, 560, msg_color, 320);
        }
        draw_text(renderer, text_cache, font_small, "font: " + font_source, 35, 585, COLOR_MUTED, 320);
        if (!thumb_fetch_status.empty()) {
            draw_text(renderer, text_cache, font_small, thumb_fetch_status, 35, 605, COLOR_MUTED, 320);
        }
        if (!debug_line.empty()) {
            draw_text(renderer, text_cache, font_small, debug_line, 35, 625, COLOR_MUTED, 320);
        }

        // Results panel
        draw_rect(renderer, RESULTS_X, RESULTS_Y, RESULTS_W, RESULTS_H, COLOR_PANEL);
        draw_rect(renderer, RESULTS_X, RESULTS_Y, RESULTS_W, RESULTS_H, COLOR_BORDER, false);

        std::string results_header = "RESULTS (" + filter_label(current_filter) + " - " +
                                      (has_loaded_once ? std::to_string(carts.size()) : std::string("0")) +
                                      " found, page " + std::to_string(current_page) + ")";
        draw_text(renderer, text_cache, font_bold, results_header, 420, 75, COLOR_WHITE);

        if (is_loading) {
            draw_loading_overlay(renderer, text_cache, font_bold, frame_counter);
        } else {
            if (carts.empty() && has_loaded_once) {
                draw_text(renderer, text_cache, font_body, "No results. Try a different filter or search term.",
                          440, 300, COLOR_MUTED);
            } else if (!has_loaded_once) {
                draw_text(renderer, text_cache, font_body, "Press Y to load carts from the BBS.",
                          440, 300, COLOR_MUTED);
            }

            for (size_t i = scroll_offset; i < carts.size() && i < (size_t)(scroll_offset + 4); i++) {
                size_t vis_idx = i - scroll_offset;
                int col = vis_idx % 2;
                int row = vis_idx / 2;
                int card_x = 420 + (col * 415);
                int card_y = 120 + (row * 240);

                bool is_selected = (focus == Focus::RESULTS && (int)i == selected_cart);
                SDL_Color card_border = is_selected ? COLOR_ACCENT : COLOR_BORDER;

                draw_rect(renderer, card_x, card_y, 400, 220, COLOR_BG);
                draw_rect(renderer, card_x, card_y, 400, 220, card_border, false);

                draw_rect(renderer, card_x + 10, card_y + 10, 160, 160, COLOR_PANEL);
                draw_rect(renderer, card_x + 10, card_y + 10, 160, 160, COLOR_BORDER, false);
                auto thumb_it = thumb_textures.find(carts[i].tid);
                if (thumb_it != thumb_textures.end() && thumb_it->second) {
                    SDL_Rect dst = { card_x + 10, card_y + 10, 160, 160 };
                    SDL_RenderCopy(renderer, thumb_it->second, nullptr, &dst);
                } else {
                    draw_text(renderer, text_cache, font_small, "PICO-8", card_x + 55, card_y + 80, COLOR_MUTED);
                }

                draw_text(renderer, text_cache, font_bold, carts[i].title, card_x + 180, card_y + 12, COLOR_WHITE, 210);
                std::string author_line = "by " + (carts[i].author.empty() ? std::string("?") : carts[i].author);
                draw_text(renderer, text_cache, font_small, author_line, card_x + 180, card_y + 40, COLOR_MUTED, 210);

                if (carts[i].downloading) {
                    float pct = g_download_progress.load();
                    draw_rect(renderer, card_x + 180, card_y + 130, 210, 40, COLOR_BG);
                    draw_rect(renderer, card_x + 180, card_y + 130, (int)(210 * pct), 40, COLOR_ACCENT);
                    draw_rect(renderer, card_x + 180, card_y + 130, 210, 40, COLOR_BORDER, false);
                    char pct_buf[8];
                    snprintf(pct_buf, sizeof(pct_buf), "%d%%", (int)(pct * 100));
                    draw_text(renderer, text_cache, font_small, pct_buf, card_x + 340, card_y + 141, COLOR_WHITE);
                } else {
                    SDL_Color btn_bg = COLOR_ACCENT;
                    draw_rect(renderer, card_x + 180, card_y + 130, 210, 40, btn_bg);
                    draw_text(renderer, text_cache, font_bold, "DOWNLOAD", card_x + 250, card_y + 141, COLOR_WHITE);
                }
            }

            if (scroll_offset > 0) {
                draw_text(renderer, text_cache, font_small, "\x18 more above", RESULTS_X + RESULTS_W - 100, RESULTS_Y + 10, COLOR_MUTED);
            }
            if (scroll_offset + 4 < (int)carts.size()) {
                draw_text(renderer, text_cache, font_small, "\x19 more below", RESULTS_X + RESULTS_W - 100, RESULTS_Y + RESULTS_H - 20, COLOR_MUTED);
            }
        }

        // Footer
        draw_rect(renderer, 0, 680, SCREEN_WIDTH, 40, COLOR_PANEL);
        draw_rect(renderer, 0, 680, SCREEN_WIDTH, 1, COLOR_BORDER);
        draw_text(renderer, text_cache, font_small,
                  "A SELECT   B BACK   Y REFRESH   L/R PAGE   X EDIT PATH   UP/DOWN SCROLL",
                  20, 690, COLOR_MUTED);

        SDL_RenderPresent(renderer);
        frame_counter++;
    }

    // Cleanup
    stop_detail_worker();

    g_list_fetch_active.store(false);
    if (g_list_fetch_thread.joinable()) g_list_fetch_thread.join();

    if (g_download_thread.joinable()) g_download_thread.join();

    for (auto& kv : thumb_textures) SDL_DestroyTexture(kv.second);
    text_cache.clear();
    if (font_header) TTF_CloseFont(font_header);
    if (font_bold) TTF_CloseFont(font_bold);
    if (font_body) TTF_CloseFont(font_body);
    if (font_small) TTF_CloseFont(font_small);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    curl_global_cleanup();
    romfsExit();
    socketExit();

    return 0;
}
    };
    std::string font_regular_path, font_bold_path;
    TTF_Font* font_header = nullptr;
    TTF_Font* font_bold   = nullptr;
    TTF_Font* font_body   = nullptr;
    TTF_Font* font_small  = nullptr;
    for (const char* p : bold_candidates) {
        font_header = TTF_OpenFont(p, 24);
        if (font_header) { font_bold_path = p; break; }
    }
    if (!font_bold_path.empty()) font_bold = TTF_OpenFont(font_bold_path.c_str(), 20);
    for (const char* p : regular_candidates) {
        font_body = TTF_OpenFont(p, 18);
        if (font_body) { font_regular_path = p; break; }
    }
    if (!font_regular_path.empty()) font_small = TTF_OpenFont(font_regular_path.c_str(), 15);

    bool fonts_ok = (font_header && font_bold && font_body && font_small);
    std::string font_source = font_regular_path.empty() ? "none" :
        (font_regular_path.find("romfs:") == 0 ? "romfs (auto)" : "sdmc (manual copy)");
    TextCache text_cache;

    load_config();
    ensure_download_dir();

    std::vector<CartItem> carts;
    std::unordered_map<std::string, SDL_Texture*> thumb_textures;

    CartFilter current_filter = CartFilter::Popular;
    std::string search_text;
    int current_page = 1;
    int selected_filter = 2;
    int selected_cart = 0;
    int scroll_offset = 0;
    Focus focus = Focus::FILTERS;
    std::string status_message = "Press Y to load carts.";
    bool status_is_error = false;
    bool has_loaded_once = false;
    std::string thumb_fetch_status;
    std::string debug_line;

    const char* filters_label[] = { "FEATURED", "NEW", "POPULAR", "TOP RATED" };
    const CartFilter filters_value[] = { CartFilter::Featured, CartFilter::New, CartFilter::Popular, CartFilter::TopRated };

    auto apply_fetched_list = [&](bool reset_page) {
        if (reset_page) current_page = 1;
        text_cache.clear();
        selected_cart = 0;
        scroll_offset = 0;

        if (g_fetch_error_flag) {
            status_message = g_fetch_error;
            status_is_error = true;
            carts.clear();
        } else if (!g_fetch_error.empty()) {
            status_message = g_fetch_error;
            status_is_error = false;
            carts.clear();
        } else {
            carts = std::move(g_fetched_carts);
            status_message = std::to_string(carts.size()) + " carts found.";
            status_is_error = false;
            if (!carts.empty()) start_detail_worker(carts);
        }
        has_loaded_once = true;
    };

    auto refresh_list = [&](bool reset_page) {
        if (reset_page) {
            current_page = 1;
            for (auto& kv : thumb_textures) SDL_DestroyTexture(kv.second);
            thumb_textures.clear();
        }
        selected_cart = 0;
        scroll_offset = 0;
        stop_detail_worker();
        launch_list_fetch(current_filter, search_text, current_page);
    };

    int frame_counter = 0;
    bool running = true;
    while (appletMainLoop() && running) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) running = false;

        // --- Background fetch completed? ---
        if (g_list_fetch_done.load()) {
            g_list_fetch_done.store(false);
            apply_fetched_list(false);
        }

        // --- Apply detail results from background worker (main thread only) ---
        {
            std::lock_guard<std::mutex> lk(g_detail_queue_mtx);
            for (auto& res : g_detail_queue) {
                // Find matching cart in main vector
                for (auto& c : carts) {
                    if (c.tid != res.tid) continue;
                    c.author = std::move(res.author);
                    c.download_url = std::move(res.download_url);
                    c.thumbnail_url = std::move(res.thumbnail_url);
                    c.detail_resolved = true;
                    break;
                }
                // Create SDL texture from thumb bytes (main thread only)
                if (!res.thumb_bytes.empty() && thumb_textures.find(res.tid) == thumb_textures.end()) {
                    SDL_RWops* rw = SDL_RWFromMem(res.thumb_bytes.data(), (int)res.thumb_bytes.size());
                    SDL_Surface* surf = IMG_Load_RW(rw, 1);
                    if (surf) {
                        thumb_textures[res.tid] = SDL_CreateTextureFromSurface(renderer, surf);
                        SDL_FreeSurface(surf);
                    }
                }
            }
            g_detail_queue.clear();
        }

        bool is_loading = g_list_fetch_active.load();

        if (!is_loading) {
            if (kDown & HidNpadButton_Down) {
                if (focus == Focus::SEARCH) focus = Focus::FILTERS;
                else if (focus == Focus::FILTERS) {
                    if (selected_filter < 3) selected_filter++;
                    else focus = Focus::PATH;
                } else if (focus == Focus::RESULTS && !carts.empty()) {
                    if ((size_t)(selected_cart + 2) < carts.size()) {
                        selected_cart += 2;
                        if (selected_cart >= scroll_offset + 4) scroll_offset += 2;
                    }
                }
            }
            if (kDown & HidNpadButton_Up) {
                if (focus == Focus::FILTERS) {
                    if (selected_filter > 0) selected_filter--;
                    else focus = Focus::SEARCH;
                } else if (focus == Focus::PATH) {
                    focus = Focus::FILTERS;
                } else if (focus == Focus::RESULTS && !carts.empty()) {
                    if (selected_cart - 2 >= 0) {
                        selected_cart -= 2;
                        if (selected_cart < scroll_offset) scroll_offset -= 2;
                        if (scroll_offset < 0) scroll_offset = 0;
                    }
                }
            }
            if (kDown & HidNpadButton_Right) {
                if (focus == Focus::SEARCH || focus == Focus::FILTERS || focus == Focus::PATH) {
                    focus = Focus::RESULTS;
                } else if (focus == Focus::RESULTS && !carts.empty()) {
                    if ((selected_cart % 2 == 0) && (size_t)(selected_cart + 1) < carts.size()) {
                        selected_cart++;
                        if (selected_cart >= scroll_offset + 4) scroll_offset += 2;
                    }
                }
            }
            if (kDown & HidNpadButton_Left) {
                if (focus == Focus::RESULTS) {
                    if (selected_cart % 2 == 1) {
                        selected_cart--;
                        if (selected_cart < scroll_offset) scroll_offset -= 2;
                        if (scroll_offset < 0) scroll_offset = 0;
                    } else {
                        focus = Focus::FILTERS;
                    }
                }
            }

            if (kDown & HidNpadButton_B) {
                if (focus == Focus::RESULTS) focus = Focus::FILTERS;
            }

            if (kDown & HidNpadButton_Y) {
                refresh_list(true);
            }

            if (kDown & HidNpadButton_L) {
                if (current_page > 1) { current_page--; refresh_list(false); }
            }
            if (kDown & HidNpadButton_R) {
                current_page++;
                refresh_list(false);
            }

            if (kDown & HidNpadButton_X) {
                std::string new_path = prompt_keyboard("Download folder (e.g. sdmc:/pico-8/carts/)", g_config.download_path);
                if (!new_path.empty()) {
                    if (new_path.back() != '/') new_path += '/';
                    g_config.download_path = new_path;
                    save_config();
                    ensure_download_dir();
                }
            }

            if (kDown & HidNpadButton_A) {
                if (focus == Focus::SEARCH) {
                    search_text = prompt_keyboard("Search Lexaloffle BBS...", search_text);
                    refresh_list(true);
                } else if (focus == Focus::FILTERS) {
                    current_filter = filters_value[selected_filter];
                    refresh_list(true);
                } else if (focus == Focus::PATH) {
                    std::string new_path = prompt_keyboard("Download folder", g_config.download_path);
                    if (!new_path.empty()) {
                        if (new_path.back() != '/') new_path += '/';
                        g_config.download_path = new_path;
                        save_config();
                        ensure_download_dir();
                    }
                } else if (focus == Focus::RESULTS && !carts.empty() && (size_t)selected_cart < carts.size()) {
                    CartItem& item = carts[selected_cart];
                    if (!item.detail_resolved) {
                        status_message = "Looking up download link...";
                        status_is_error = false;
                        resolve_cart_detail(item);
                        // On-demand thumb fetch for immediate feedback
                        if (!item.thumbnail_url.empty() && thumb_textures.find(item.tid) == thumb_textures.end()) {
                            std::vector<unsigned char> bytes;
                            if (http_get_binary(item.thumbnail_url, bytes)) {
                                SDL_RWops* rw = SDL_RWFromMem(bytes.data(), (int)bytes.size());
                                SDL_Surface* surf = IMG_Load_RW(rw, 1);
                                if (surf) {
                                    thumb_textures[item.tid] = SDL_CreateTextureFromSurface(renderer, surf);
                                    SDL_FreeSurface(surf);
                                }
                            }
                        }
                    }
                    if (item.download_url.empty()) {
                        status_message = "Couldn't find a direct download link for \"" + item.title +
                                          "\" - try it from lexaloffle.com instead.";
                        status_is_error = true;
                    } else if (!g_download_active.load()) {
                        ensure_download_dir();
                        std::string dest = g_config.download_path + sanitize_filename(item.title) + ".p8.png";
                        std::string url = item.download_url;
                        g_download_progress = 0.0f;
                        g_download_bytes = 0;
                        g_download_active = true;
                        g_download_label = item.title;
                        g_downloading_index = selected_cart;
                        item.downloading = true;
                        if (g_download_thread.joinable()) g_download_thread.join();
                        g_download_thread = std::thread([url, dest]() {
                            bool ok = download_file(url, dest, &g_download_progress, &g_download_bytes);
                            g_download_success = ok;
                            g_download_active = false;
                        });
                    }
                }
            }
        }

        // --- Pick up finished download ---
        if (!g_download_active.load() && g_downloading_index >= 0) {
            if ((size_t)g_downloading_index < carts.size()) {
                carts[g_downloading_index].downloading = false;
                status_message = (g_download_success.load() ? "Downloaded " : "Failed to download ") + g_download_label;
                status_is_error = !g_download_success.load();
            }
            g_downloading_index = -1;
            if (g_download_thread.joinable()) g_download_thread.join();
        }

        // --- Debug line ---
        if (focus == Focus::RESULTS && !carts.empty() && (size_t)selected_cart < carts.size()) {
            CartItem& sel = carts[selected_cart];
            if (sel.detail_resolved) {
                debug_line = "cover: " + (sel.thumbnail_url.empty() ? std::string("not found") : std::string("found")) +
                             "  |  cart: " + (sel.download_url.empty() ? std::string("not found") : std::string("found"));
            }
        }

        // ================================================================
        // RENDER
        // ================================================================
        SDL_SetRenderDrawColor(renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, 255);
        SDL_RenderClear(renderer);

        draw_rect(renderer, 0, 0, SCREEN_WIDTH, 6, fonts_ok ? SDL_Color{0,200,0,255} : SDL_Color{220,0,0,255});

        // Header
        draw_rect(renderer, 0, 0, SCREEN_WIDTH, 45, COLOR_PANEL);
        draw_rect(renderer, 0, 44, SCREEN_WIDTH, 1, COLOR_BORDER);
        draw_text(renderer, text_cache, font_header, "PICO-8 Cart Browser & Downloader v1.1", 20, 8, COLOR_WHITE);
        {
            const char* credit = "by tehrzky";
            int cw = 0, ch = 0;
            if (font_body) TTF_SizeUTF8(font_body, credit, &cw, &ch);
            draw_text(renderer, text_cache, font_body, credit, SCREEN_WIDTH - cw - 20, 13, COLOR_MUTED);
        }

        // Sidebar
        draw_rect(renderer, SIDEBAR_X, 60, SIDEBAR_W, 610, COLOR_PANEL);
        draw_rect(renderer, SIDEBAR_X, 60, SIDEBAR_W, 610, COLOR_BORDER, false);
        draw_text(renderer, text_cache, font_bold, "Filters & Search", 35, 75, COLOR_WHITE);

        {
            SDL_Color box_color = (focus == Focus::SEARCH) ? COLOR_SELECTED : COLOR_BG;
            draw_rect(renderer, 35, 105, 330, 45, box_color);
            draw_rect(renderer, 35, 105, 330, 45, COLOR_BORDER, false);
            std::string shown = search_text.empty() ? "Search Lexaloffle BBS..." : search_text;
            SDL_Color txt_color = search_text.empty() ? COLOR_MUTED : COLOR_WHITE;
            draw_text(renderer, text_cache, font_body, shown, 48, 118, txt_color, 300);
        }

        for (int i = 0; i < 4; i++) {
            bool is_current = (filters_value[i] == current_filter);
            bool is_focused = (focus == Focus::FILTERS && selected_filter == i);
            SDL_Color btn_color = is_focused ? COLOR_SELECTED : (is_current ? SDL_Color{40, 70, 55, 255} : COLOR_BG);
            SDL_Color border_color = is_current ? COLOR_ACCENT : COLOR_BORDER;
            int by = 200 + (i * 55);
            draw_rect(renderer, 35, by, 330, 45, btn_color);
            draw_rect(renderer, 35, by, 330, 45, border_color, false);
            draw_text(renderer, text_cache, font_bold, filters_label[i], 50, by + 12, COLOR_WHITE);
        }

        {
            SDL_Color box_color = (focus == Focus::PATH) ? COLOR_SELECTED : COLOR_BG;
            draw_rect(renderer, 35, 460, 330, 45, box_color);
            draw_rect(renderer, 35, 460, 330, 45, COLOR_BORDER, false);
            draw_text(renderer, text_cache, font_small, "Download path:", 45, 440, COLOR_MUTED);
            draw_text(renderer, text_cache, font_body, g_config.download_path, 48, 473, COLOR_WHITE, 300);
        }

        {
            SDL_Color msg_color = status_is_error ? COLOR_ERROR : COLOR_MUTED;
            draw_text(renderer, text_cache, font_small, status_message, 35, 560, msg_color, 320);
        }
        draw_text(renderer, text_cache, font_small, "font: " + font_source, 35, 585, COLOR_MUTED, 320);
        if (!thumb_fetch_status.empty()) {
            draw_text(renderer, text_cache, font_small, thumb_fetch_status, 35, 605, COLOR_MUTED, 320);
        }
        if (!debug_line.empty()) {
            draw_text(renderer, text_cache, font_small, debug_line, 35, 625, COLOR_MUTED, 320);
        }

        // Results panel
        draw_rect(renderer, RESULTS_X, RESULTS_Y, RESULTS_W, RESULTS_H, COLOR_PANEL);
        draw_rect(renderer, RESULTS_X, RESULTS_Y, RESULTS_W, RESULTS_H, COLOR_BORDER, false);

        std::string results_header = "RESULTS (" + filter_label(current_filter) + " - " +
                                      (has_loaded_once ? std::to_string(carts.size()) : std::string("0")) +
                                      " found, page " + std::to_string(current_page) + ")";
        draw_text(renderer, text_cache, font_bold, results_header, 420, 75, COLOR_WHITE);

        if (is_loading) {
            draw_loading_overlay(renderer, text_cache, font_bold, frame_counter);
        } else {
            if (carts.empty() && has_loaded_once) {
                draw_text(renderer, text_cache, font_body, "No results. Try a different filter or search term.",
                          440, 300, COLOR_MUTED);
            } else if (!has_loaded_once) {
                draw_text(renderer, text_cache, font_body, "Press Y to load carts from the BBS.",
                          440, 300, COLOR_MUTED);
            }

            for (size_t i = scroll_offset; i < carts.size() && i < (size_t)(scroll_offset + 4); i++) {
                size_t vis_idx = i - scroll_offset;
                int col = vis_idx % 2;
                int row = vis_idx / 2;
                int card_x = 420 + (col * 415);
                int card_y = 120 + (row * 240);

                bool is_selected = (focus == Focus::RESULTS && (int)i == selected_cart);
                SDL_Color card_border = is_selected ? COLOR_ACCENT : COLOR_BORDER;

                draw_rect(renderer, card_x, card_y, 400, 220, COLOR_BG);
                draw_rect(renderer, card_x, card_y, 400, 220, card_border, false);

                draw_rect(renderer, card_x + 10, card_y + 10, 160, 160, COLOR_PANEL);
                draw_rect(renderer, card_x + 10, card_y + 10, 160, 160, COLOR_BORDER, false);
                auto thumb_it = thumb_textures.find(carts[i].tid);
                if (thumb_it != thumb_textures.end() && thumb_it->second) {
                    SDL_Rect dst = { card_x + 10, card_y + 10, 160, 160 };
                    SDL_RenderCopy(renderer, thumb_it->second, nullptr, &dst);
                } else {
                    draw_text(renderer, text_cache, font_small, "PICO-8", card_x + 55, card_y + 80, COLOR_MUTED);
                }

                draw_text(renderer, text_cache, font_bold, carts[i].title, card_x + 180, card_y + 12, COLOR_WHITE, 210);
                std::string author_line = "by " + (carts[i].author.empty() ? std::string("?") : carts[i].author);
                draw_text(renderer, text_cache, font_small, author_line, card_x + 180, card_y + 40, COLOR_MUTED, 210);

                if (carts[i].downloading) {
                    float pct = g_download_progress.load();
                    draw_rect(renderer, card_x + 180, card_y + 130, 210, 40, COLOR_BG);
                    draw_rect(renderer, card_x + 180, card_y + 130, (int)(210 * pct), 40, COLOR_ACCENT);
                    draw_rect(renderer, card_x + 180, card_y + 130, 210, 40, COLOR_BORDER, false);
                    char pct_buf[8];
                    snprintf(pct_buf, sizeof(pct_buf), "%d%%", (int)(pct * 100));
                    draw_text(renderer, text_cache, font_small, pct_buf, card_x + 340, card_y + 141, COLOR_WHITE);
                } else {
                    SDL_Color btn_bg = COLOR_ACCENT;
                    draw_rect(renderer, card_x + 180, card_y + 130, 210, 40, btn_bg);
                    draw_text(renderer, text_cache, font_bold, "DOWNLOAD", card_x + 250, card_y + 141, COLOR_WHITE);
                }
            }

            if (scroll_offset > 0) {
                draw_text(renderer, text_cache, font_small, "\x18 more above", RESULTS_X + RESULTS_W - 100, RESULTS_Y + 10, COLOR_MUTED);
            }
            if (scroll_offset + 4 < (int)carts.size()) {
                draw_text(renderer, text_cache, font_small, "\x19 more below", RESULTS_X + RESULTS_W - 100, RESULTS_Y + RESULTS_H - 20, COLOR_MUTED);
            }
        }

        // Footer
        draw_rect(renderer, 0, 680, SCREEN_WIDTH, 40, COLOR_PANEL);
        draw_rect(renderer, 0, 680, SCREEN_WIDTH, 1, COLOR_BORDER);
        draw_text(renderer, text_cache, font_small,
                  "A SELECT   B BACK   Y REFRESH   L/R PAGE   X EDIT PATH   UP/DOWN SCROLL",
                  20, 690, COLOR_MUTED);

        SDL_RenderPresent(renderer);
        frame_counter++;
    }

    // Cleanup
    stop_detail_worker();

    g_list_fetch_active.store(false);
    if (g_list_fetch_thread.joinable()) g_list_fetch_thread.join();

    if (g_download_thread.joinable()) g_download_thread.join();

    for (auto& kv : thumb_textures) SDL_DestroyTexture(kv.second);
    text_cache.clear();
    if (font_header) TTF_CloseFont(font_header);
    if (font_bold) TTF_CloseFont(font_bold);
    if (font_body) TTF_CloseFont(font_body);
    if (font_small) TTF_CloseFont(font_small);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    curl_global_cleanup();
    romfsExit();
    socketExit();

    return 0;
}
