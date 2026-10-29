#include "hotkeys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <X11/Xutil.h>

/* Default configuration table matching Task 7 specifications */
static const HotkeyBinding DEFAULT_BINDINGS[HOTKEY_COUNT] = {
    { HOTKEY_MOVE_START,      "move_start",      ControlMask,              XK_a,    "Ctrl+A" },
    { HOTKEY_MOVE_END,        "move_end",        ControlMask,              XK_e,    "Ctrl+E" },
    { HOTKEY_DELETE_TO_START, "delete_to_start", ControlMask,              XK_u,    "Ctrl+U" },
    { HOTKEY_DELETE_TO_END,   "delete_to_end",   ControlMask,              XK_k,    "Ctrl+K" },
    { HOTKEY_DELETE_WORD,     "delete_word",     ControlMask,              XK_w,    "Ctrl+W" },
    { HOTKEY_CLEAR_SCREEN,    "clear_screen",    ControlMask,              XK_l,    "Ctrl+L" },
    { HOTKEY_EXIT_SHELL,      "exit_shell",      ControlMask,              XK_d,    "Ctrl+D" },
    { HOTKEY_COPY,            "copy",            ControlMask | ShiftMask,  XK_c,    "Ctrl+Shift+C" },
    { HOTKEY_PASTE,           "paste",           ControlMask | ShiftMask,  XK_v,    "Ctrl+Shift+V" },
    { HOTKEY_HISTORY_PREV,    "history_prev",    0,                        XK_Up,   "Up" },
    { HOTKEY_HISTORY_NEXT,    "history_next",    0,                        XK_Down, "Down" }
};

/**
 * @brief Helper function to trim leading and trailing whitespace from a string in-place.
 */
static char *trim_whitespace(char *str) {
    if (!str) return NULL;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

/**
 * @brief Checks if two KeySyms represent the same logical key, ignoring letter case.
 */
static int keysyms_equal(KeySym k1, KeySym k2) {
    if (k1 == k2) return 1;
    /* Compare ASCII lower-case equivalents */
    if (k1 >= 'A' && k1 <= 'Z') k1 += ('a' - 'A');
    if (k2 >= 'A' && k2 <= 'Z') k2 += ('a' - 'A');
    return (k1 == k2);
}

/**
 * @brief Parses a combination string like "Ctrl+Shift+C" or "Up" into modifiers and KeySym.
 *
 * @param[in] combo Null-terminated string representation.
 * @param[out] out_mods Populated modifier bitmask.
 * @param[out] out_sym Populated X11 KeySym.
 * @return 0 on success, -1 on invalid key name.
 */
static int parse_combo_string(const char *combo, unsigned int *out_mods, KeySym *out_sym) {
    if (!combo || !out_mods || !out_sym) return -1;

    unsigned int mods = 0;
    char buf[128];
    strncpy(buf, combo, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = strtok(buf, "+");
    char *last_token = NULL;

    while (token != NULL) {
        char *trimmed = trim_whitespace(token);
        char *next = strtok(NULL, "+");

        if (next == NULL) {
            /* This is the final component (the key name itself) */
            last_token = trimmed;
            break;
        }

        if (strcasecmp(trimmed, "Ctrl") == 0 || strcasecmp(trimmed, "Control") == 0) {
            mods |= ControlMask;
        } else if (strcasecmp(trimmed, "Shift") == 0) {
            mods |= ShiftMask;
        } else if (strcasecmp(trimmed, "Alt") == 0 || strcasecmp(trimmed, "Mod1") == 0) {
            mods |= Mod1Mask;
        } else {
            /* Unknown modifier */
            return -1;
        }
        token = next;
    }

    if (!last_token || strlen(last_token) == 0) {
        return -1;
    }

    /* Convert key name to X11 KeySym */
    KeySym sym = XStringToKeysym(last_token);
    if (sym == NoSymbol) {
        /* Check single character conversion fallback */
        if (strlen(last_token) == 1) {
            char c = last_token[0];
            if (c >= 'A' && c <= 'Z') sym = (KeySym)(c + ('a' - 'A'));
            else sym = (KeySym)c;
        } else {
            return -1;
        }
    }

    /* Normalize lower-case for alpha keysyms */
    if (sym >= 'A' && sym <= 'Z') {
        sym += ('a' - 'A');
    }

    *out_mods = mods;
    *out_sym = sym;
    return 0;
}

/**
 * @brief Checks for conflicts across all hotkey bindings.
 *
 * If any two actions are bound to the exact same combination, reports an error to stderr
 * and safely reverts both conflicting actions to their factory defaults.
 */
static void check_and_resolve_conflicts(HotkeyConfig *cfg) {
    for (int i = 0; i < HOTKEY_COUNT; i++) {
        for (int j = i + 1; j < HOTKEY_COUNT; j++) {
            if (cfg->bindings[i].modifiers == cfg->bindings[j].modifiers &&
                keysyms_equal(cfg->bindings[i].keysym, cfg->bindings[j].keysym)) {
                fprintf(stderr,
                        "terminator: ~/.terminatorrc: hotkey conflict between '%s' and '%s'; reverting to defaults\n",
                        cfg->bindings[i].action_name, cfg->bindings[j].action_name);
                /* Revert both actions to default */
                cfg->bindings[i] = DEFAULT_BINDINGS[i];
                cfg->bindings[j] = DEFAULT_BINDINGS[j];
            }
        }
    }
}

/**
 * @brief Writes a default template config file ~/.terminatorrc if not present.
 */
static void write_default_config_file(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) return;

    fprintf(fp, "# ~/.terminatorrc - Custom Hotkey Bindings for Terminator\n");
    fprintf(fp, "# Format: action_name = [Ctrl+][Shift+][Alt+]Key\n");
    fprintf(fp, "# Changes take effect automatically without recompilation.\n\n");

    for (int i = 0; i < HOTKEY_COUNT; i++) {
        fprintf(fp, "%-16s = %s\n", DEFAULT_BINDINGS[i].action_name, DEFAULT_BINDINGS[i].default_desc);
    }

    fclose(fp);
}

void hotkeys_init(HotkeyConfig *cfg) {
    if (!cfg) return;

    /* 1. Seed with factory defaults */
    for (int i = 0; i < HOTKEY_COUNT; i++) {
        cfg->bindings[i] = DEFAULT_BINDINGS[i];
    }

    /* 2. Locate configuration file in user's home directory */
    const char *home = getenv("HOME");
    if (!home) return;

    char rc_path[512];
    snprintf(rc_path, sizeof(rc_path), "%s/.terminatorrc", home);

    /* If file does not exist, create a starter template with defaults */
    if (access(rc_path, F_OK) != 0) {
        write_default_config_file(rc_path);
        return;
    }

    /* 3. Parse existing configuration file */
    FILE *fp = fopen(rc_path, "r");
    if (!fp) return;

    char line[256];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        char *p = trim_whitespace(line);

        /* Skip comments and blank lines */
        if (*p == '\0' || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *action_str = trim_whitespace(p);
        char *combo_str = trim_whitespace(eq + 1);

        /* Match action_str to a known action */
        int found_idx = -1;
        for (int i = 0; i < HOTKEY_COUNT; i++) {
            if (strcasecmp(action_str, cfg->bindings[i].action_name) == 0) {
                found_idx = i;
                break;
            }
        }

        if (found_idx < 0) {
            fprintf(stderr, "terminator: %s:%d: unknown action '%s'\n", rc_path, line_num, action_str);
            continue;
        }

        /* Parse combination */
        unsigned int mods = 0;
        KeySym sym = NoSymbol;
        if (parse_combo_string(combo_str, &mods, &sym) != 0) {
            fprintf(stderr, "terminator: %s:%d: invalid key combination '%s' for '%s'; using default\n",
                    rc_path, line_num, combo_str, action_str);
            continue;
        }

        cfg->bindings[found_idx].modifiers = mods;
        cfg->bindings[found_idx].keysym = sym;
    }

    fclose(fp);

    /* 4. Validate and resolve any mutual conflicts */
    check_and_resolve_conflicts(cfg);
}

int hotkeys_match(const HotkeyConfig *cfg, const XKeyEvent *event, KeySym keysym) {
    if (!cfg || !event) return -1;

    /* Clean modifier mask (ignore CapsLock, NumLock, ScrollLock) */
    unsigned int clean_mods = event->state & (ControlMask | ShiftMask | Mod1Mask);

    for (int i = 0; i < HOTKEY_COUNT; i++) {
        if (cfg->bindings[i].modifiers == clean_mods) {
            if (keysyms_equal(cfg->bindings[i].keysym, keysym)) {
                return cfg->bindings[i].action;
            }
        }
    }

    return -1;
}
