#ifndef GUI_H
#define GUI_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <sys/types.h>
#include <limits.h>
#include <pthread.h>
#include "shell.h"
#include "hotkeys.h"
#include "history.h"

#define MAX_TABS 8
#define MAX_LINE_CHARS 1024
#define MAX_BUFFER_LINES 2000
#define MAX_TAB_HISTORY 1000
#define TAB_BAR_HEIGHT 36

/**
 * @brief Represents the visual category of a line in the terminal buffer.
 */
typedef enum {
    LINE_TYPE_OUTPUT,       /**< Standard program output (rendered in light output color) */
    LINE_TYPE_PROMPT_INPUT  /**< Command line containing red prompt and green user command */
} LineType;

/**
 * @brief Represents a single line of text stored in a tab's scrollback buffer.
 */
typedef struct {
    char text[MAX_LINE_CHARS]; /**< Null-terminated string content of the line */
    LineType type;             /**< Visual style/type for rendering */
    int prompt_len;            /**< Length of the prompt prefix in characters, if applicable */
} TermLine;

/**
 * @brief Represents a single independent terminal tab.
 *
 * Each tab maintains:
 * - Its own scrollback text buffer.
 * - Current directory (`cwd`).
 * - Active input line being typed by the user.
 * - Non-blocking pipes to receive asynchronous output from commands.
 * - Independent ShellContext for exit status ($?) and variables.
 */
typedef struct {
    int tab_id;                             /**< Unique numeric identifier for this tab (1-indexed) */
    int use_process_mode;                   /**< 0 = Thread Mode (worker thread), 1 = Process Mode (child process) */
    pthread_t worker_thread;                /**< POSIX worker thread handle (Thread Mode) */
    pid_t tab_pid;                          /**< Dedicated child process PID (Process Mode) */
    int to_tab_fd;                          /**< Pipe write descriptor (GUI -> Tab) */
    int is_thread_alive;                    /**< 1 if worker thread is actively running */
    char cwd[PATH_MAX];                     /**< Tab's current working directory */
    TermLine lines[MAX_BUFFER_LINES];       /**< Scrollback line buffer */
    int num_lines;                          /**< Total number of lines currently stored in buffer */
    int scroll_offset;                      /**< Vertical scroll offset (0 = scrolled to bottom) */
    char input_buffer[MAX_LINE_CHARS];      /**< Line currently being typed by the user */
    int input_len;                          /**< Number of characters in input_buffer */
    int cursor_pos;                         /**< Current cursor position within input_buffer */
    ShellContext shell_ctx;                 /**< Shell context specific to this tab */
    int is_running_cmd;                     /**< Flag indicating whether a foreground command is currently running */
    pid_t running_pid;                      /**< PID of the running child command (or 0 if idle) */
    pid_t running_pgid;                     /**< Process group ID of the running foreground command */
    char running_cmd[MAX_LINE_CHARS];       /**< Command line string of active foreground command */
    int out_pipe_fd;                        /**< Read end of the pipe capturing stdout/stderr from child process */
    int is_multiline;                       /**< Non-zero if currently reading multiline continuation input ('> ') */
    char multiline_accum[MAX_LINE_CHARS * 4]; /**< Accumulated multi-line command string before execution */
    int history_pos;                        /**< Active position when browsing history (-1 when at bottom prompt) */
    char saved_input[MAX_LINE_CHARS];       /**< Preserves in-progress typed text when browsing history */
} Tab;

/**
 * @brief Main GUI application structure holding all X11 state and tabs.
 */
typedef struct {
    Display *display;        /**< Connection handle to the X server */
    int screen;              /**< Default screen number on the display */
    Window window;           /**< Top-level X11 window handle */
    Pixmap back_buffer;      /**< Off-screen pixmap for double-buffered flicker-free rendering */
    XFontStruct *font;       /**< Loaded monospace font structure */
    int font_width;          /**< Width in pixels of a single character */
    int font_height;         /**< Height in pixels of a text line (ascent + descent) */
    int font_ascent;         /**< Font ascent in pixels (baseline distance from top of character) */
    int win_width;           /**< Current window width in pixels */
    int win_height;          /**< Current window height in pixels */

    /* Graphical Contexts (GC) with distinct colors */
    GC gc_bg;                /**< Graphics Context for black background */
    GC gc_prompt;            /**< Graphics Context for Red prompt ("user@terminator>") */
    GC gc_input;             /**< Graphics Context for Neon Green typed user commands */
    GC gc_output;            /**< Graphics Context for Command output text */
    GC gc_cursor;            /**< Graphics Context for drawing the blinking / solid block cursor */
    GC gc_tab_bar_bg;        /**< Graphics Context for the tab bar strip background */
    GC gc_tab_text;          /**< Graphics Context for inactive tab labels */
    GC gc_tab_active_bg;     /**< Graphics Context for active tab highlight background */
    GC gc_tab_active_text;   /**< Graphics Context for active tab label text */
    GC gc_scrollbar;         /**< Graphics Context for the right-side scrollbar indicator */

    /* Tab management */
    Tab *tabs[MAX_TABS];     /**< Array of active Tab pointers */
    int num_tabs;            /**< Count of currently open tabs */
    int active_tab_idx;      /**< Zero-based index of the currently displayed tab */
    Atom wm_delete_window;   /**< X11 Atom for intercepting window manager close ('X' button) */

    /* Hotkeys & Clipboard */
    HotkeyConfig hotkeys;    /**< Active hotkey bindings table */
    Atom atom_clipboard;     /**< X11 CLIPBOARD selection atom */
    Atom atom_utf8_string;   /**< X11 UTF8_STRING target atom */
    Atom atom_targets;       /**< X11 TARGETS atom */
    Atom atom_terminator_sel;/**< Custom property atom for receiving pasted selection data */
    char clipboard_data[MAX_LINE_CHARS * 4]; /**< Internal clipboard buffer for copy/paste */
    int clipboard_len;       /**< Length of active clipboard content */

    int is_running;          /**< Application loop flag (1 = running, 0 = exit application) */
    int use_process_mode;    /**< 0 = Thread Mode (default), 1 = Process Mode (useProcess=True) */
} GuiApp;

/**
 * @brief Initializes the X11 GUI window, allocates color GCs, loads fonts, and creates the first tab.
 *
 * @param[out] app Pointer to GuiApp structure to initialize.
 * @param[in] width Initial window width in pixels.
 * @param[in] height Initial window height in pixels.
 * @param[in] title Window title string displayed on titlebar.
 * @param[in] use_process_mode 0 for Thread Mode, 1 for Process Mode.
 * @return 0 on success, or -1 on initialization failure (e.g. Cannot open X display).
 */
int gui_init(GuiApp *app, int width, int height, const char *title, int use_process_mode);

/**
 * @brief Frees all X11 resources, closed tabs, GCs, pixmaps, and closes connection to X server.
 *
 * @param[in,out] app Pointer to GuiApp to clean up.
 */
void gui_cleanup(GuiApp *app);

/**
 * @brief Creates a new terminal tab with its own shell context and initial directory.
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in] initial_cwd Starting directory for the tab (or NULL to use current process directory).
 * @return Index of the newly created tab, or -1 if maximum tabs limit reached.
 */
int gui_create_tab(GuiApp *app, const char *initial_cwd);

/**
 * @brief Closes an existing tab by index and frees its memory.
 *
 * If the active tab is closed, switches to an adjacent tab.
 * If the last remaining tab is closed, sets app->is_running = 0 to exit Terminator.
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in] tab_idx Zero-based index of the tab to close.
 */
void gui_close_tab(GuiApp *app, int tab_idx);

/**
 * @brief Switches the active view to the specified tab.
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in] tab_idx Zero-based index of the tab to activate.
 */
void gui_switch_tab(GuiApp *app, int tab_idx);

/**
 * @brief Appends raw text output (e.g. read from child process pipe) to a tab's buffer.
 *
 * Parses newline characters ('\n') to break text into distinct lines, wraps long lines,
 * and scrolls the buffer if the maximum line count is reached.
 *
 * @param[in,out] tab Pointer to the target Tab.
 * @param[in] text Pointer to buffer containing incoming character bytes.
 * @param[in] len Number of bytes in text buffer.
 */
void tab_append_output(Tab *tab, const char *text, int len);

/**
 * @brief Submits the current input buffer in the active tab for execution.
 *
 * Spawns a child process or executes a built-in command, wiring stdout and stderr
 * to an asynchronous pipe monitored by the X11 event loop.
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in,out] tab Pointer to Tab where Enter was pressed.
 */
void tab_submit_command(GuiApp *app, Tab *tab);

/**
 * @brief Handles X11 KeyPress events (typing characters, Enter, Backspace, arrows, hotkeys).
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in] event Pointer to XKeyEvent.
 */
void gui_handle_key_press(GuiApp *app, XKeyEvent *event);

/**
 * @brief Handles X11 ButtonPress events (clicking tabs on the tab bar, '+' button, etc.).
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in] event Pointer to XButtonEvent.
 */
void gui_handle_button_press(GuiApp *app, XButtonEvent *event);

/**
 * @brief Redraws the entire terminal window: tab bar, scrollback text, prompt, and input line.
 *
 * Implements double-buffering using an off-screen pixmap to ensure zero flicker.
 *
 * @param[in,out] app Pointer to GuiApp.
 */
void gui_redraw(GuiApp *app);

/**
 * @brief Main non-blocking event loop using poll().
 *
 * Sleeps until either an X11 event arrives on ConnectionNumber(display) OR data is ready
 * to be read on any running tab's output pipe. Redraws only when state changes.
 *
 * @param[in,out] app Pointer to GuiApp.
 */
void gui_run_loop(GuiApp *app);

#endif /* GUI_H */
