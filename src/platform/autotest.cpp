// Scripted input for unattended runs (the Sonic R 3DS port's format).
//
//   AUTOTEST.TXT next to the executable, one command per line:
//     WHEN <text>                  start a phase when <text> appears on stderr
//     <frame> <frames> KEY[+KEY]   hold keys for <frames> (30 Hz wall clock)
//     <frame> <frames> TOUCH x y   touch at (x, y) for <frames>
//     <frame> SHOT <name>          log "AUTOTEST shot <name>" (the harness screenshots)
//     <frame> LOG <text>           log the text
//     <frame> EXIT                 exit(0)
//   Frames count from the start of the current phase.
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "recomp3ds_internal.h"

namespace {

constexpr int kMaxCmds = 256;
constexpr int kMaxPhases = 32;
enum Kind { KEYS, TOUCH, SHOT, LOG, EXIT };
struct Cmd { int frame, frames; u32 keys; Kind kind; int phase; int tx, ty; char text[48]; };

Cmd g_cmds[kMaxCmds];
int g_count = 0;
char g_trigger[kMaxPhases][48];
int g_phases = 1;
int g_phase = 0;
volatile int g_pending_phase = -1;
u64 g_phase_start_ms = 0;
int g_last_frame = -1;
u32 g_keys = 0;
bool g_touch = false;
int g_touch_x = 0, g_touch_y = 0;

u32 key_bit(const char* name) {
    static const struct { const char* n; u32 k; } tab[] = {
        {"A",KEY_A},{"B",KEY_B},{"X",KEY_X},{"Y",KEY_Y},{"L",KEY_L},{"R",KEY_R},
        {"ZL",KEY_ZL},{"ZR",KEY_ZR},{"START",KEY_START},{"SELECT",KEY_SELECT},
        {"UP",KEY_DUP},{"DOWN",KEY_DDOWN},{"LEFT",KEY_DLEFT},{"RIGHT",KEY_DRIGHT},
    };
    for (auto& e : tab) {
        if (strcasecmp(e.n, name) == 0) return e.k;
    }
    return 0;
}

}   // namespace

void recomp3ds::autotest_load(const char* base_path) {
    char path[192];
    snprintf(path, sizeof(path), "%s/AUTOTEST.TXT", base_path);
    FILE* fp = fopen(path, "r");
    if (fp == nullptr) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp) && g_count < kMaxCmds) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (strncasecmp(line, "WHEN ", 5) == 0) {
            if (g_phases < kMaxPhases) {
                strncpy(g_trigger[g_phases], line + 5, sizeof(g_trigger[0]) - 1);
                char* nl = strpbrk(g_trigger[g_phases], "\r\n");
                if (nl) *nl = '\0';
                g_phases++;
            }
            continue;
        }
        char a[48], b[48], c[96];
        int n = sscanf(line, "%47s %47s %95[^\n]", a, b, c);
        if (n < 2) continue;
        Cmd* cmd = &g_cmds[g_count];
        memset(cmd, 0, sizeof(*cmd));
        cmd->frame = atoi(a);
        cmd->phase = g_phases - 1;
        if (strcasecmp(b, "SHOT") == 0)      { cmd->kind = SHOT; if (n > 2) strncpy(cmd->text, c, sizeof(cmd->text) - 1); }
        else if (strcasecmp(b, "LOG") == 0)  { cmd->kind = LOG;  if (n > 2) strncpy(cmd->text, c, sizeof(cmd->text) - 1); }
        else if (strcasecmp(b, "EXIT") == 0) { cmd->kind = EXIT; }
        else if (n > 2 && strncasecmp(c, "TOUCH", 5) == 0) {
            cmd->kind = TOUCH; cmd->frames = atoi(b);
            sscanf(c + 5, "%d %d", &cmd->tx, &cmd->ty);
        }
        else if (n > 2) {
            cmd->kind = KEYS; cmd->frames = atoi(b);
            char* tok = strtok(c, "+ \r\n");
            while (tok) { cmd->keys |= key_bit(tok); tok = strtok(nullptr, "+ \r\n"); }
        }
        else continue;
        g_count++;
    }
    fclose(fp);
    fprintf(stderr, "AUTOTEST: %d commands, %d phases loaded\n", g_count, g_phases);
}

// Called with every stderr line (from the log hook, under its lock).
void recomp3ds::autotest_scan_line(const char* line) {
    if (g_count == 0) return;
    for (int p = g_phase + 1; p < g_phases; p++) {
        if (g_trigger[p][0] != '\0' && strstr(line, g_trigger[p]) != nullptr) {
            g_pending_phase = p;
            break;
        }
    }
}

// Called once per input poll; returns the keys the script is holding.
u32 recomp3ds::autotest_tick() {
    if (g_count == 0) return 0;
    if (g_pending_phase >= 0) {
        g_phase = g_pending_phase;
        g_pending_phase = -1;
        g_phase_start_ms = osGetTime();
        g_last_frame = -1;
        fprintf(stderr, "AUTOTEST: phase %d (%s)\n", g_phase, g_trigger[g_phase]);
    }
    if (g_phase_start_ms == 0) g_phase_start_ms = osGetTime();
    int f = (int)((osGetTime() - g_phase_start_ms) * 30 / 1000);
    int prev = g_last_frame;
    g_last_frame = f;
    u32 keys = 0;
    g_touch = false;
    for (int i = 0; i < g_count; i++) {
        Cmd* c = &g_cmds[i];
        if (c->phase != g_phase) continue;
        bool fired = (prev < c->frame && f >= c->frame);
        switch (c->kind) {
            case KEYS:
                if (f >= c->frame && f < c->frame + c->frames) keys |= c->keys;
                if (fired) fprintf(stderr, "AUTOTEST: keys %08lX for %d frames\n", (unsigned long)c->keys, c->frames);
                break;
            case TOUCH:
                if (f >= c->frame && f < c->frame + c->frames) { g_touch = true; g_touch_x = c->tx; g_touch_y = c->ty; }
                break;
            case SHOT: if (fired) fprintf(stderr, "AUTOTEST shot %s\n", c->text); break;
            case LOG:  if (fired) fprintf(stderr, "AUTOTEST: %s\n", c->text); break;
            case EXIT: if (fired) { fprintf(stderr, "AUTOTEST: exit\n"); recomp3ds::request_exit(); } break;
        }
    }
    g_keys = keys;
    return keys;
}

bool recomp3ds::autotest_touch(int* x, int* y) {
    if (g_touch) { *x = g_touch_x; *y = g_touch_y; }
    return g_touch;
}
