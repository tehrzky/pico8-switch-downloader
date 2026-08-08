#include <switch.h>
#include <stdio.h>
#include "config.hpp"
#include "downloader.hpp"

int main(int argc, char **argv) {
    consoleInit(NULL);
    socketInitializeDefault();

    // Initialize controller state
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    load_config();

    printf("\x1b[1;1HPICO-8 Downloader & Browser");
    printf("\x1b[3;1HCurrent Path: %s", g_config.download_path.c_str());
    printf("\x1b[5;1HPress A to Download Demo Cartridge (Celeste)");
    printf("\x1b[6;1HPress + to Exit");

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) break;

        if (kDown & HidNpadButton_A) {
            printf("\x1b[8;1HDownloading cartridge...");
            consoleUpdate(NULL);
            
            std::string dest = g_config.download_path + "celeste.p8.png";
            bool success = download_file("https://www.lexaloffle.com/bbs/cdata/0/celeste.p8.png", dest);

            if (success) {
                printf("\x1b[9;1HDownload successful!  ");
            } else {
                printf("\x1b[9;1HDownload failed.      ");
            }
        }

        consoleUpdate(NULL);
    }

    socketExit();
    consoleExit(NULL);
    return 0;
}
