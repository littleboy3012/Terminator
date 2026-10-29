#ifndef HOTKEYS_H
#define HOTKEYS_H

#include <X11/Xlib.h>
#include <X11/keysym.h>

/**
 * @brief Enum of all configurable actions required by Task 7.
 */
typedef enum {
    HOTKEY_MOVE_START = 0,      /**< Ctrl+A: Move cursor to start of line */
    HOTKEY_MOVE_END,            /**< Ctrl+E: Move cursor to end of line */
    HOTKEY_DELETE_TO_START,     /**< Ctrl+U: Delete from cursor to start of line */
    HOTKEY_DELETE_TO_END,       /**< Ctrl+K: Delete from cursor to end of line */
    HOTKEY_DELETE_WORD,         /**< Ctrl+W: Delete previous word before cursor */
    HOTKEY_CLEAR_SCREEN,        /**< Ctrl+L: Clear the screen */
    HOTKEY_EXIT_SHELL,          /**< Ctrl+D: Exit the shell / close active tab */
    HOTKEY_COPY,                /**< Ctrl+Shift+C: Copy text to X11 clipboard */
    HOTKEY_PASTE,               /**< Ctrl+Shift+V: Paste text from X11 clipboard */
    HOTKEY_HISTORY_PREV,        /**< Up: Browse previous command in history */
    HOTKEY_HISTORY_NEXT,        /**< Down: Browse next command in history */
    HOTKEY_COUNT
} HotkeyAction;

/**
 * @brief Represents a single key combination binding.
 */
typedef struct {
    HotkeyAction action;        /**< Action ID */
    const char *action_name;    /**< Action string key in ~/.terminatorrc */
    unsigned int modifiers;     /**< Modifier bitmask (ControlMask, ShiftMask, Mod1Mask) */
    KeySym keysym;              /**< X11 KeySym (e.g. XK_A, XK_Up) */
    const char *default_desc;   /**< Human-readable default description (e.g. "Ctrl+A") */
} HotkeyBinding;

/**
 * @brief Global hotkey table holding active bindings.
 */
typedef struct {
    HotkeyBinding bindings[HOTKEY_COUNT];
} HotkeyConfig;

/**
 * @brief Initializes hotkeys, loading custom bindings from ~/.terminatorrc if present.
 *
 * If ~/.terminatorrc does not exist, it creates a template with default values.
 * If any binding in the config is invalid or conflicts with another, a warning is printed
 * and it safely falls back to the default combination.
 *
 * @param[out] cfg Pointer to HotkeyConfig structure to populate.
 */
void hotkeys_init(HotkeyConfig *cfg);

/**
 * @brief Matches an incoming X11 KeyPress event against the configured hotkey bindings.
 *
 * @param[in] cfg Pointer to active HotkeyConfig.
 * @param[in] event Pointer to XKeyEvent.
 * @param[in] keysym Looked-up KeySym for the event.
 * @return The matched HotkeyAction, or -1 if no hotkey matched.
 */
int hotkeys_match(const HotkeyConfig *cfg, const XKeyEvent *event, KeySym keysym);

#endif /* HOTKEYS_H */
