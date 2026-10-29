// launcher96: starts an rt64-3ds game's hidden 96 MB CIA on an Old 3DS.
// A last resort: no game ships one (the owner's choice, 2026-10-28; 80 MB
// with 85% of core 1 is the Old 3DS's setup).
//
// The HOME Menu cannot start a title that asks for the 96 MB layout: it
// restarts the console into that layout itself and crashes at its own start
// (it wants a 13 MB heap of the 12 MB the layout leaves the system). A game
// built with add_cia(... OLD3DS_MODE 96MB HIDDEN PRODUCT "<code>-96") is
// left off the HOME Menu; this .3dsx, from the Homebrew Launcher, lists the
// installed ones (their product code ends in "-96") and has NS restart the
// console straight into the chosen one with the 96 MB layout
// (NS_RebootToTitle, as Luma3DS's autoboot does). No HOME Menu runs behind
// the game then: the game's HOME and POWER open its quit prompt, and
// quitting restarts the console into the HOME Menu (recomp3ds_run.cpp).
#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Title {
    u64 id;
    std::string name;
};

// The title's short name (English) from its SMDH, read from its content.
std::string title_name(u64 id) {
    static const u32 kIconPath[] = { 0x0, 0x0, 0x2, 0x6E6F6369, 0x0 };   // ExeFS "icon"
    u32 archive_path[] = { (u32)id, (u32)(id >> 32), MEDIATYPE_SD, 0x0 };
    const FS_Path apath = { PATH_BINARY, sizeof(archive_path), archive_path };
    const FS_Path fpath = { PATH_BINARY, sizeof(kIconPath), kIconPath };
    Handle file = 0;
    std::string name;
    if (R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SAVEDATA_AND_CONTENT, apath, fpath, FS_OPEN_READ, 0))) {
        // SMDH: 8 bytes of header, then 16 titles of 0x200 bytes (short
        // name: 0x40 UTF-16 units); English is the second.
        u16 short_name[0x40] = {};
        u32 read = 0;
        if (R_SUCCEEDED(FSFILE_Read(file, &read, 8 + 0x200, short_name, sizeof(short_name))) && read == sizeof(short_name)) {
            for (u16 c : short_name) {
                if (c == 0) break;
                name.push_back(c < 0x80 ? (char)c : '?');
            }
        }
        FSFILE_Close(file);
    }
    if (name.empty()) {
        char tid[24];
        snprintf(tid, sizeof(tid), "%016llX", (unsigned long long)id);
        name = tid;
    }
    return name;
}

std::vector<Title> find_titles() {
    std::vector<Title> found;
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(MEDIATYPE_SD, &count)) || count == 0) return found;
    std::vector<u64> ids(count);
    u32 read = 0;
    if (R_FAILED(AM_GetTitleList(&read, MEDIATYPE_SD, count, ids.data()))) return found;
    for (u32 i = 0; i < read; i++) {
        if ((ids[i] >> 32) != 0x00040000) continue;        // applications only
        char code[16 + 1] = {};
        if (R_FAILED(AM_GetTitleProductCode(MEDIATYPE_SD, ids[i], code))) continue;
        const size_t n = strnlen(code, 16);
        if (n < 3 || strcmp(code + n - 3, "-96") != 0) continue;
        found.push_back({ ids[i], title_name(ids[i]) });
    }
    return found;
}

}   // namespace

int main() {
    gfxInitDefault();
    PrintConsole top, bottom;
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);
    amInit();
    bool is_new_3ds = false;
    APT_CheckNew3DS(&is_new_3ds);
    const std::vector<Title> titles = find_titles();

    consoleSelect(&bottom);
    printf("Starts a game installed for the Old\n3DS's 96 MB layout. The console\nrestarts into it, with no HOME Menu\nbehind it: the game's HOME and POWER\nopen its quit prompt, and quitting\nrestarts into the HOME Menu.\n");
    if (is_new_3ds) printf("\nA New 3DS has no need of this: install\nthe game's normal CIA instead.\n");
    printf("\nD-Pad: choose   A: start   START: exit\n");

    int sel = 0;
    auto draw = [&]() {
        consoleSelect(&top);
        consoleClear();
        printf("96 MB launcher (rt64-3ds)\n\n");
        if (titles.empty()) {
            printf("No 96 MB titles installed.\n\nInstall a game's 96 MB CIA (its name\nends in -o3ds96) with FBI.\n");
            return;
        }
        for (int i = 0; i < (int)titles.size(); i++) {
            printf("%s %.44s\n", i == sel ? ">" : " ", titles[i].name.c_str());
        }
    };
    draw();

    bool restarting = false;
    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        if (down & KEY_START) break;
        if (!restarting && !titles.empty()) {
            if (down & KEY_DUP) { sel = (sel + (int)titles.size() - 1) % (int)titles.size(); draw(); }
            if (down & KEY_DDOWN) { sel = (sel + 1) % (int)titles.size(); draw(); }
            if (down & KEY_A) {
                consoleSelect(&top);
                printf("\nRestarting into %.30s...\n", titles[sel].name.c_str());
                // APPMEMTYPE 2: 96 MB on an Old 3DS; a New 3DS keeps its own (6).
                Result rc = nsInit();
                if (R_SUCCEEDED(rc)) {
                    rc = NS_RebootToTitle(MEDIATYPE_SD, titles[sel].id, is_new_3ds ? 6 : 2);
                    nsExit();
                }
                if (R_FAILED(rc)) printf("The restart was refused (%08lX).\n", (unsigned long)rc);
                else restarting = true;
            }
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    amExit();
    gfxExit();
    return 0;
}
