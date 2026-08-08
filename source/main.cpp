#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <vector>
#include <string>
#include "config.hpp"
#include "downloader.hpp"

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

struct CartItem {
    std::string title;
    std::string author;
    std::string url;
    std::string preview_url;
    bool downloading = false;
};

void draw_rect(SDL_Renderer* renderer, int x, int y, int w, int h, SDL_Color color, bool fill = true) {
    SDL_Rect rect = { x, y, w, h };
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    if (fill) {
        SDL_RenderFillRect(renderer, &rect);
    } else {
        SDL_RenderDrawRect(renderer, &rect);
    }
}

int main(int argc, char **argv) {
    // 1. Initialize Switch Systems & SDL2 Subsystems
    socketInitializeDefault();
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

    load_config();

    // Sample data feed matching UI grid
    std::vector<CartItem> carts = {
        { "CELESTE Classic", "NoelFB", "https://www.lexaloffle.com/bbs/cdata/0/celeste.p8.png", "" },
        { "DUNGEON EXPLORER", "PicoDev", "https://www.lexaloffle.com/bbs/cdata/0/dungeon.p8.png", "" },
        { "STARFIGHTER", "PicoDev", "https://www.lexaloffle.com/bbs/cdata/0/starfighter.p8.png", "" },
        { "PICO-8 RACER", "Zepto", "https://www.lexaloffle.com/bbs/cdata/0/racer.p8.png", "" }
    };

    int selected_filter = 2; // Default to "POPULAR"
    int selected_cart = 0;
    bool running = true;

    // 2. Main Render Loop
    while (appletMainLoop() && running) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) running = false;

        // Navigation
        if (kDown & HidNpadButton_Right) selected_cart = (selected_cart + 1) % carts.size();
        if (kDown & HidNpadButton_Left)  selected_cart = (selected_cart - 1 + carts.size()) % carts.size();

        // Download Action
        if (kDown & HidNpadButton_A) {
            carts[selected_cart].downloading = true;
            std::string dest = g_config.download_path + carts[selected_cart].title + ".p8.png";
            download_file(carts[selected_cart].url, dest);
            carts[selected_cart].downloading = false;
        }

        // Render Background
        SDL_SetRenderDrawColor(renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, 255);
        SDL_RenderClear(renderer);

        // --- TOP HEADER BAR ---
        draw_rect(renderer, 0, 0, SCREEN_WIDTH, 45, COLOR_PANEL);
        draw_rect(renderer, 0, 44, SCREEN_WIDTH, 1, COLOR_BORDER);

        // --- LEFT SIDEBAR (Filters & Settings) ---
        draw_rect(renderer, 20, 60, 360, 610, COLOR_PANEL);
        draw_rect(renderer, 20, 60, 360, 610, COLOR_BORDER, false);

        // Filter Options
        const char* filters[] = { "FEATURED", "NEW", "POPULAR", "TOP RATED" };
        for (int i = 0; i < 4; i++) {
            SDL_Color btn_color = (i == selected_filter) ? COLOR_SELECTED : COLOR_BG;
            draw_rect(renderer, 35, 170 + (i * 55), 330, 45, btn_color);
            draw_rect(renderer, 35, 170 + (i * 55), 330, 45, COLOR_BORDER, false);
        }

        // Download Path Container
        draw_rect(renderer, 35, 580, 330, 45, COLOR_BG);
        draw_rect(renderer, 35, 580, 330, 45, COLOR_BORDER, false);

        // --- RIGHT GRID AREA (Cartridge Cards) ---
        draw_rect(renderer, 400, 60, 860, 610, COLOR_PANEL);
        draw_rect(renderer, 400, 60, 860, 610, COLOR_BORDER, false);

        // Render 2x2 Grid Items
        for (size_t i = 0; i < carts.size(); i++) {
            int col = i % 2;
            int row = i / 2;
            int card_x = 420 + (col * 415);
            int card_y = 120 + (row * 240);

            SDL_Color card_border = (static_cast<int>(i) == selected_cart) ? COLOR_ACCENT : COLOR_BORDER;
            
            // Card Base
            draw_rect(renderer, card_x, card_y, 400, 220, COLOR_BG);
            draw_rect(renderer, card_x, card_y, 400, 220, card_border, false);

            // Thumbnail Preview Box (Left inside card)
            draw_rect(renderer, card_x + 10, card_y + 10, 160, 160, COLOR_PANEL);
            draw_rect(renderer, card_x + 10, card_y + 10, 160, 160, COLOR_BORDER, false);

            // Download Button (Right bottom inside card)
            SDL_Color btn_bg = carts[i].downloading ? COLOR_MUTED : COLOR_ACCENT;
            draw_rect(renderer, card_x + 180, card_y + 130, 210, 40, btn_bg);
        }

        // --- BOTTOM FOOTER (Button Bar) ---
        draw_rect(renderer, 0, 680, SCREEN_WIDTH, 40, COLOR_PANEL);
        draw_rect(renderer, 0, 680, SCREEN_WIDTH, 1, COLOR_BORDER);

        SDL_RenderPresent(renderer);
    }

    // Cleanup
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    socketExit();

    return 0;
}
