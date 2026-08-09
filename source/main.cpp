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
#include <unordered_map>
#include <algorithm>
#include "config.hpp"
#include "downloader.hpp"
#include "scraper.hpp"

// Screen Resolution
constexpr int SCREEN_WIDTH = 1280;
constexpr int SCREEN_HEIGHT = 720;

// Colors (RGBA)
const SDL_Color COLOR_BG       = { 20,  20,  20, 255 };
const SDL_Color COLOR_PANEL    = { 32,  32,  32, 255 };
const SDL_Color COLOR_BORDER   = { 60,  60,  60, 255 };
const SDL_Color COLOR_ACCENT   = {  0, 180, 120, 255 };
const SDL_Color COLOR_WHITE    = { 255, 255, 255, 255 };
const SDL_Color COLOR_MUTED    = { 160, 160, 160, 255 };
const SDL_Color COLOR_SELECTED = { 45,  90, 140, 255 };
const SDL_Color COLOR_ERROR    = { 200, 70,  70, 255 };

enum class Focus { SEARCH, FILTERS, PATH, RESULTS };

// --- Globals shared with the background download thread ---
static std::atomic<bool>  g_download_active{false};
static std::atomic<float> g_download_progress{0.0f};
static std::atomic<long>  g_download_bytes{0};
static std::atomic<bool>  g_download_success{false};
static std::thread        g_download_thread;
static std::string        g_download_label;
static int                g_downloading_index = -1;

// --- Text texture cache (avoids re-rendering the same string every frame) ---
struct TextCache {
    std::unordered_map<std::string, SDL_Texture*> map;

    SDL_Texture* get(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, SDL_Color color) {
        if (text.empty()) return nullptr;
        char key[16];
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

// Draws text and returns its width in pixels (0 if it couldn't be rendered).
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

// Opens the Switch's on-screen keyboard and returns what the user typed
// (or the original string if they cancelled).
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

int main(int argc, char **argv) {
    // 1. Initialize Switch Systems & SDL2 Subsystems
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

    // Fonts bundled in romfs (see romfs/ folder + --romfsdir at build time).
    TTF_Font* font_header = TTF_OpenFont("romfs:/PTSans-Bold.ttf", 24);
    TTF_Font* font_bold   = TTF_OpenFont("romfs:/PTSans-Bold.ttf", 20);
    TTF_Font* font_body   = TTF_OpenFont("romfs:/PTSans-Regular.ttf", 18);
    TTF_Font* font_small  = TTF_OpenFont("romfs:/PTSans-Regular.ttf", 15);
    TextCache text_cache;

    load_config();
    ensure_download_dir();

    std::vector<CartItem> carts;
    std::unordered_map<std::string, SDL_Texture*> thumb_textures; // keyed by tid

    CartFilter current_filter = CartFilter::Popular;
    std::string search_text;
    int current_page = 1;
    int selected_filter = 2; // index into filters[] matching Popular
    int selected_cart = 0;
    Focus focus = Focus::FILTERS;
    std::string status_message = "Press Y to load carts from the BBS.";
    bool status_is_error = false;
    bool has_loaded_once = false;

    const char* filters_label[] = { "FEATURED", "NEW", "POPULAR", "TOP RATED" };
    const CartFilter filters_value[] = { CartFilter::Featured, CartFilter::New, CartFilter::Popular, CartFilter::TopRated };

    auto refresh_list = [&](bool reset_page) {
        if (reset_page) current_page = 1;
        text_cache.clear();
        for (auto& kv : thumb_textures) SDL_DestroyTexture(kv.second);
        thumb_textures.clear();
        selected_cart = 0;

        // Draw a quick "loading" frame before the blocking network call.
        SDL_SetRenderDrawColor(renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, 255);
        SDL_RenderClear(renderer);
        draw_text(renderer, text_cache, font_bold, "Loading carts from lexaloffle.com...",
                  SCREEN_WIDTH / 2 - 160, SCREEN_HEIGHT / 2, COLOR_WHITE);
        SDL_RenderPresent(renderer);

        std::string err;
        bool ok = fetch_cart_list(current_filter, search_text, current_page, carts, err);
        has_loaded_once = true;
        if (!ok) {
            status_message = err;
            status_is_error = true;
        } else if (!err.empty()) {
            status_message = err; // "no carts found" - not fatal, just informational
            status_is_error = false;
        } else {
            status_message = std::to_string(carts.size()) + " carts found.";
            status_is_error = false;
        }
    };

    // 2. Main Render Loop
    bool running = true;
    while (appletMainLoop() && running) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) running = false;

        // --- Focus navigation ---
        if (kDown & HidNpadButton_Down) {
            if (focus == Focus::SEARCH) focus = Focus::FILTERS;
            else if (focus == Focus::FILTERS) {
                if (selected_filter < 3) selected_filter++;
                else focus = Focus::PATH;
            } else if (focus == Focus::RESULTS && !carts.empty()) {
                if ((size_t)(selected_cart + 2) < carts.size()) selected_cart += 2;
            }
        }
        if (kDown & HidNpadButton_Up) {
            if (focus == Focus::FILTERS) {
                if (selected_filter > 0) selected_filter--;
                else focus = Focus::SEARCH;
            } else if (focus == Focus::PATH) {
                focus = Focus::FILTERS;
            } else if (focus == Focus::RESULTS && !carts.empty()) {
                if (selected_cart - 2 >= 0) selected_cart -= 2;
            }
        }
        if (kDown & HidNpadButton_Right) {
            if (focus == Focus::SEARCH || focus == Focus::FILTERS || focus == Focus::PATH) {
                focus = Focus::RESULTS;
            } else if (focus == Focus::RESULTS && !carts.empty()) {
                if ((selected_cart % 2 == 0) && (size_t)(selected_cart + 1) < carts.size()) selected_cart++;
            }
        }
        if (kDown & HidNpadButton_Left) {
            if (focus == Focus::RESULTS) {
                if (selected_cart % 2 == 1) selected_cart--;
                else focus = Focus::FILTERS;
            }
        }

        // --- B: step focus back toward the sidebar ---
        if (kDown & HidNpadButton_B) {
            if (focus == Focus::RESULTS) focus = Focus::FILTERS;
        }

        // --- Y: refresh current feed ---
        if (kDown & HidNpadButton_Y) {
            refresh_list(true);
        }

        // --- L/R: page through results ---
        if (kDown & HidNpadButton_L) {
            if (current_page > 1) { current_page--; refresh_list(false); }
        }
        if (kDown & HidNpadButton_R) {
            current_page++;
            refresh_list(false);
        }

        // --- X: quick-edit download path ---
        if (kDown & HidNpadButton_X) {
            std::string new_path = prompt_keyboard("Download folder (e.g. sdmc:/pico-8/carts/)", g_config.download_path);
            if (!new_path.empty()) {
                if (new_path.back() != '/') new_path += '/';
                g_config.download_path = new_path;
                save_config();
                ensure_download_dir();
            }
        }

        // --- A: activate whatever is focused ---
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
                    SDL_SetRenderDrawColor(renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, 255);
                    SDL_RenderClear(renderer);
                    draw_text(renderer, text_cache, font_bold, status_message, 440, 340, COLOR_WHITE);
                    SDL_RenderPresent(renderer);
                    resolve_cart_detail(item);
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

        // --- Pick up a finished download ---
        if (!g_download_active.load() && g_downloading_index >= 0) {
            if ((size_t)g_downloading_index < carts.size()) {
                carts[g_downloading_index].downloading = false;
                status_message = (g_download_success.load() ? "Downloaded " : "Failed to download ") + g_download_label;
                status_is_error = !g_download_success.load();
            }
            g_downloading_index = -1;
            if (g_download_thread.joinable()) g_download_thread.join();
        }

        // --- Lazily resolve detail + thumbnail for the currently selected card ---
        if (focus == Focus::RESULTS && !carts.empty() && (size_t)selected_cart < carts.size()) {
            CartItem& sel = carts[selected_cart];
            if (!sel.detail_resolved) {
                resolve_cart_detail(sel);
            }
            if (!sel.thumbnail_url.empty() && thumb_textures.find(sel.tid) == thumb_textures.end()) {
                std::vector<unsigned char> bytes;
                if (http_get_binary(sel.thumbnail_url, bytes)) {
                    SDL_RWops* rw = SDL_RWFromMem(bytes.data(), (int)bytes.size());
                    SDL_Surface* surf = IMG_Load_RW(rw, 1);
                    if (surf) {
                        thumb_textures[sel.tid] = SDL_CreateTextureFromSurface(renderer, surf);
                        SDL_FreeSurface(surf);
                    }
                }
            }
        }

        // Render Background
        SDL_SetRenderDrawColor(renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, 255);
        SDL_RenderClear(renderer);

        // --- TOP HEADER BAR ---
        draw_rect(renderer, 0, 0, SCREEN_WIDTH, 45, COLOR_PANEL);
        draw_rect(renderer, 0, 44, SCREEN_WIDTH, 1, COLOR_BORDER);
        draw_text(renderer, text_cache, font_header, "PICO-8 Cart Browser & Downloader v1.0", 20, 8, COLOR_WHITE);

        // --- LEFT SIDEBAR (Filters & Settings) ---
        draw_rect(renderer, 20, 60, 360, 610, COLOR_PANEL);
        draw_rect(renderer, 20, 60, 360, 610, COLOR_BORDER, false);
        draw_text(renderer, text_cache, font_bold, "Filters & Search", 35, 75, COLOR_WHITE);

        // Search box
        {
            SDL_Color box_color = (focus == Focus::SEARCH) ? COLOR_SELECTED : COLOR_BG;
            draw_rect(renderer, 35, 105, 330, 45, box_color);
            draw_rect(renderer, 35, 105, 330, 45, COLOR_BORDER, false);
            std::string shown = search_text.empty() ? "Search Lexaloffle BBS..." : search_text;
            SDL_Color txt_color = search_text.empty() ? COLOR_MUTED : COLOR_WHITE;
            draw_text(renderer, text_cache, font_body, shown, 48, 118, txt_color, 300);
        }

        // Filter Options
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

        // Download Path Container
        {
            SDL_Color box_color = (focus == Focus::PATH) ? COLOR_SELECTED : COLOR_BG;
            draw_rect(renderer, 35, 460, 330, 45, box_color);
            draw_rect(renderer, 35, 460, 330, 45, COLOR_BORDER, false);
            draw_text(renderer, text_cache, font_small, "Download path:", 45, 440, COLOR_MUTED);
            draw_text(renderer, text_cache, font_body, g_config.download_path, 48, 473, COLOR_WHITE, 300);
        }

        // Status / error message
        {
            SDL_Color msg_color = status_is_error ? COLOR_ERROR : COLOR_MUTED;
            draw_text(renderer, text_cache, font_small, status_message, 35, 600, msg_color, 320);
        }

        // --- RIGHT GRID AREA (Cartridge Cards) ---
        draw_rect(renderer, 400, 60, 860, 610, COLOR_PANEL);
        draw_rect(renderer, 400, 60, 860, 610, COLOR_BORDER, false);

        std::string results_header = "RESULTS (" + filter_label(current_filter) + " - " +
                                      (has_loaded_once ? std::to_string(carts.size()) : std::string("0")) +
                                      " found, page " + std::to_string(current_page) + ")";
        draw_text(renderer, text_cache, font_bold, results_header, 420, 75, COLOR_WHITE);

        if (carts.empty() && has_loaded_once) {
            draw_text(renderer, text_cache, font_body, "No results. Try a different filter or search term.",
                      440, 300, COLOR_MUTED);
        } else if (!has_loaded_once) {
            draw_text(renderer, text_cache, font_body, "Press Y to load carts from the BBS.",
                      440, 300, COLOR_MUTED);
        }

        // Render 2x2-visible Grid Items (up to 4 cards per page shown at a time)
        for (size_t i = 0; i < carts.size() && i < 4; i++) {
            int col = i % 2;
            int row = i / 2;
            int card_x = 420 + (col * 415);
            int card_y = 120 + (row * 240);

            bool is_selected = (focus == Focus::RESULTS && (int)i == selected_cart);
            SDL_Color card_border = is_selected ? COLOR_ACCENT : COLOR_BORDER;

            // Card Base
            draw_rect(renderer, card_x, card_y, 400, 220, COLOR_BG);
            draw_rect(renderer, card_x, card_y, 400, 220, card_border, false);

            // Thumbnail Preview Box (Left inside card)
            draw_rect(renderer, card_x + 10, card_y + 10, 160, 160, COLOR_PANEL);
            draw_rect(renderer, card_x + 10, card_y + 10, 160, 160, COLOR_BORDER, false);
            auto thumb_it = thumb_textures.find(carts[i].tid);
            if (thumb_it != thumb_textures.end() && thumb_it->second) {
                SDL_Rect dst = { card_x + 10, card_y + 10, 160, 160 };
                SDL_RenderCopy(renderer, thumb_it->second, nullptr, &dst);
            } else {
                draw_text(renderer, text_cache, font_small, "PICO-8", card_x + 55, card_y + 80, COLOR_MUTED);
            }

            // Title / author (right of thumbnail)
            draw_text(renderer, text_cache, font_bold, carts[i].title, card_x + 180, card_y + 12, COLOR_WHITE, 210);
            std::string author_line = "by " + (carts[i].author.empty() ? std::string("?") : carts[i].author);
            draw_text(renderer, text_cache, font_small, author_line, card_x + 180, card_y + 40, COLOR_MUTED, 210);

            // Download Button / progress bar (Right bottom inside card)
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

        // --- BOTTOM FOOTER (Button Bar) ---
        draw_rect(renderer, 0, 680, SCREEN_WIDTH, 40, COLOR_PANEL);
        draw_rect(renderer, 0, 680, SCREEN_WIDTH, 1, COLOR_BORDER);
        draw_text(renderer, text_cache, font_small,
                  "A SELECT   B BACK   Y REFRESH FEED   L/R PAGE UP/DOWN   X EDIT DOWNLOAD PATH",
                  20, 690, COLOR_MUTED);

        SDL_RenderPresent(renderer);
    }

    // Cleanup
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
