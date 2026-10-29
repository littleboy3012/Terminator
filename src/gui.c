#include "gui.h"
#include "parser.h"
#include "builtins.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <ctype.h>
#include <X11/Xatom.h>

#define DEFAULT_FONT_NAME "fixed"
#define PROMPT_STRING "user@terminator> "

/**
 * @brief Helper function to allocate a color in X11 using 16-bit RGB values.
 *
 * @param[in] display Connection to the X server.
 * @param[in] colormap Colormap of the screen.
 * @param[in] red 16-bit red intensity (0x0000 - 0xFFFF).
 * @param[in] green 16-bit green intensity (0x0000 - 0xFFFF).
 * @param[in] blue 16-bit blue intensity (0x0000 - 0xFFFF).
 * @return Pixel value representing the allocated color.
 */
static unsigned long allocate_color(Display *display, Colormap colormap,
                                   unsigned short red, unsigned short green, unsigned short blue) {
    XColor color;
    color.red = red;
    color.green = green;
    color.blue = blue;
    color.flags = DoRed | DoGreen | DoBlue;

    if (XAllocColor(display, colormap, &color)) {
        return color.pixel;
    }

    /* Fallback to WhitePixel if color allocation fails */
    return WhitePixel(display, DefaultScreen(display));
}

/**
 * @brief Initializes the graphical user interface, creates window, sets up double buffering,
 *        and prepares graphics contexts with custom hacker color schemes.
 *
 * @param[out] app Pointer to the GuiApp structure to initialize.
 * @param[in] width Desired starting window width in pixels.
 * @param[in] height Desired starting window height in pixels.
 * @param[in] title Window title string to display on the desktop window manager bar.
 * @return 0 on success, -1 on failure (e.g. Cannot open X display).
 */
int gui_init(GuiApp *app, int width, int height, const char *title) {
    if (!app) {
        return -1;
    }
    memset(app, 0, sizeof(GuiApp));

    /* Connect to the X server (uses $DISPLAY environment variable, or falls back to :0) */
    app->display = XOpenDisplay(NULL);
    if (!app->display) {
        app->display = XOpenDisplay(":0");
    }
    if (!app->display) {
        fprintf(stderr, "terminator: cannot open X display. Is an X server or WSLg running?\n");
        return -1;
    }

    app->screen = DefaultScreen(app->display);
    app->win_width = width;
    app->win_height = height;
    Colormap colormap = DefaultColormap(app->display, app->screen);

    /* Allocate custom hacker colors:
     * - Background: Pitch Black (#000000)
     * - Prompt: Vivid Red (#FF3333) for "user@terminator>"
     * - Input: Neon Hacker Green (#33FF33) for typed commands
     * - Output: Crisp Matrix Light-Green / Gray (#CCEECC) for clean readability
     * - Cursor: Bright Green block (#33FF33)
     */
    unsigned long col_black     = BlackPixel(app->display, app->screen);
    unsigned long col_red       = allocate_color(app->display, colormap, 0xFFFF, 0x3333, 0x3333);
    unsigned long col_green     = allocate_color(app->display, colormap, 0x3333, 0xFFFF, 0x3333);
    unsigned long col_output    = allocate_color(app->display, colormap, 0xCCCC, 0xEEEE, 0xCCCC);
    unsigned long col_tab_bg    = allocate_color(app->display, colormap, 0x1818, 0x1818, 0x1818);
    unsigned long col_tab_act   = allocate_color(app->display, colormap, 0x2A2A, 0x4444, 0x2A2A);
    unsigned long col_tab_text  = allocate_color(app->display, colormap, 0x8888, 0xAAAA, 0x8888);
    unsigned long col_scroll    = allocate_color(app->display, colormap, 0x5555, 0x7777, 0x5555);

    /* Create the top-level window */
    app->window = XCreateSimpleWindow(
        app->display,
        RootWindow(app->display, app->screen),
        100, 100,
        app->win_width, app->win_height,
        1,
        col_green, /* Border color: neon green */
        col_black  /* Background: pure black */
    );

    /* Set window title and class name for window managers */
    XStoreName(app->display, app->window, title);
    XSetIconName(app->display, app->window, title);

    /* Select events we want to receive from X11:
     * - KeyPressMask: Keyboard input
     * - ButtonPressMask: Mouse clicks (tabs, + button)
     * - ExposureMask: Window exposed and needs redrawing
     * - StructureNotifyMask: Window resized or moved
     */
    XSelectInput(app->display, app->window,
                 KeyPressMask | ButtonPressMask | ExposureMask | StructureNotifyMask);

    /* Load larger, crisp monospace terminal font */
    const char *font_candidates[] = {
        "-misc-fixed-medium-r-normal--20-*-*-*-*-*-iso8859-1",
        "10x20",
        "-misc-fixed-medium-r-normal--18-*-*-*-*-*-iso8859-1",
        "9x18",
        "-misc-fixed-medium-r-normal--15-*-*-*-*-*-iso8859-1",
        "9x15",
        "fixed",
        NULL
    };
    app->font = NULL;
    for (int i = 0; font_candidates[i]; i++) {
        app->font = XLoadQueryFont(app->display, font_candidates[i]);
        if (app->font) {
            break;
        }
    }
    if (!app->font) {
        app->font = XQueryFont(app->display, XGContextFromGC(DefaultGC(app->display, app->screen)));
    }

    if (app->font) {
        app->font_width = app->font->max_bounds.width;
        if (app->font_width <= 0) app->font_width = 8;
        app->font_ascent = app->font->ascent;
        app->font_height = app->font->ascent + app->font->descent;
        if (app->font_height <= 0) app->font_height = 16;
    } else {
        app->font_width = 8;
        app->font_height = 16;
        app->font_ascent = 12;
    }

    /* Create Graphics Contexts (GC) for each text element */
    XGCValues gcv;
    memset(&gcv, 0, sizeof(gcv));
    if (app->font) gcv.font = app->font->fid;

    /* Background GC */
    gcv.foreground = col_black;
    app->gc_bg = XCreateGC(app->display, app->window, GCForeground | (app->font ? GCFont : 0), &gcv);

    /* Prompt GC (Vivid Red) */
    gcv.foreground = col_red;
    app->gc_prompt = XCreateGC(app->display, app->window, GCForeground | (app->font ? GCFont : 0), &gcv);

    /* Input GC (Neon Hacker Green) */
    gcv.foreground = col_green;
    app->gc_input = XCreateGC(app->display, app->window, GCForeground | (app->font ? GCFont : 0), &gcv);

    /* Output GC (Crisp Light Text) */
    gcv.foreground = col_output;
    app->gc_output = XCreateGC(app->display, app->window, GCForeground | (app->font ? GCFont : 0), &gcv);

    /* Cursor GC (Neon Green) */
    gcv.foreground = col_green;
    app->gc_cursor = XCreateGC(app->display, app->window, GCForeground | (app->font ? GCFont : 0), &gcv);

    /* Tab bar GCs */
    gcv.foreground = col_tab_bg;
    app->gc_tab_bar_bg = XCreateGC(app->display, app->window, GCForeground | (app->font ? GCFont : 0), &gcv);

    gcv.foreground = col_tab_act;
    app->gc_tab_active_bg = XCreateGC(app->display, app->window, GCForeground | (app->font ? GCFont : 0), &gcv);

    gcv.foreground = col_tab_text;
    app->gc_tab_text = XCreateGC(app->display, app->window, GCForeground | (app->font ? GCFont : 0), &gcv);

    gcv.foreground = col_green;
    app->gc_tab_active_text = XCreateGC(app->display, app->window, GCForeground | (app->font ? GCFont : 0), &gcv);

    /* Scrollbar GC */
    gcv.foreground = col_scroll;
    app->gc_scrollbar = XCreateGC(app->display, app->window, GCForeground, &gcv);

    /* Create off-screen back buffer for double-buffered flicker-free rendering */
    app->back_buffer = XCreatePixmap(app->display, app->window,
                                     app->win_width, app->win_height,
                                     DefaultDepth(app->display, app->screen));

    /* Register window manager delete protocol so clicking 'X' exits gracefully */
    app->wm_delete_window = XInternAtom(app->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app->display, app->window, &app->wm_delete_window, 1);

    /* Initialize X11 selection clipboard atoms */
    app->atom_clipboard = XInternAtom(app->display, "CLIPBOARD", False);
    app->atom_utf8_string = XInternAtom(app->display, "UTF8_STRING", False);
    app->atom_targets = XInternAtom(app->display, "TARGETS", False);
    app->atom_terminator_sel = XInternAtom(app->display, "TERMINATOR_SELECTION", False);
    app->clipboard_data[0] = '\0';
    app->clipboard_len = 0;

    /* Initialize Task 7 Hotkeys from ~/.terminatorrc */
    hotkeys_init(&app->hotkeys);

    /* Map (show) the window on the desktop screen */
    XMapWindow(app->display, app->window);
    XFlush(app->display);

    app->is_running = 1;

    /* Create initial Tab 1 starting in the current working directory */
    gui_create_tab(app, NULL);

    return 0;
}

/**
 * @brief Releases all allocated X11 resources, closes pipes, and frees memory.
 *
 * @param[in,out] app Pointer to GuiApp structure.
 */
void gui_cleanup(GuiApp *app) {
    if (!app || !app->display) {
        return;
    }

    /* Close and free all tabs */
    for (int i = 0; i < app->num_tabs; i++) {
        if (app->tabs[i]) {
            if (app->tabs[i]->out_pipe_fd >= 0) {
                close(app->tabs[i]->out_pipe_fd);
            }
            if (app->tabs[i]->running_pid > 0) {
                kill(app->tabs[i]->running_pid, SIGTERM);
            }
            free(app->tabs[i]);
            app->tabs[i] = NULL;
        }
    }
    app->num_tabs = 0;

    /* Free X11 Graphics Contexts and off-screen pixmap */
    if (app->back_buffer) XFreePixmap(app->display, app->back_buffer);
    if (app->gc_bg) XFreeGC(app->display, app->gc_bg);
    if (app->gc_prompt) XFreeGC(app->display, app->gc_prompt);
    if (app->gc_input) XFreeGC(app->display, app->gc_input);
    if (app->gc_output) XFreeGC(app->display, app->gc_output);
    if (app->gc_cursor) XFreeGC(app->display, app->gc_cursor);
    if (app->gc_tab_bar_bg) XFreeGC(app->display, app->gc_tab_bar_bg);
    if (app->gc_tab_text) XFreeGC(app->display, app->gc_tab_text);
    if (app->gc_tab_active_bg) XFreeGC(app->display, app->gc_tab_active_bg);
    if (app->gc_tab_active_text) XFreeGC(app->display, app->gc_tab_active_text);
    if (app->gc_scrollbar) XFreeGC(app->display, app->gc_scrollbar);

    if (app->font) XFreeFont(app->display, app->font);
    if (app->window) XDestroyWindow(app->display, app->window);

    XCloseDisplay(app->display);
    app->display = NULL;
}

/**
 * @brief Creates a new terminal tab and initializes its shell context and buffers.
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in] initial_cwd Starting path for this tab, or NULL for current directory.
 * @return Index of the created tab (0-indexed), or -1 if maximum tabs limit reached.
 */
int gui_create_tab(GuiApp *app, const char *initial_cwd) {
    if (!app || app->num_tabs >= MAX_TABS) {
        return -1;
    }

    Tab *tab = malloc(sizeof(Tab));
    if (!tab) {
        return -1;
    }
    memset(tab, 0, sizeof(Tab));

    tab->tab_id = app->num_tabs + 1;
    tab->out_pipe_fd = -1;
    tab->running_pid = 0;
    tab->is_running_cmd = 0;
    tab->num_lines = 0;
    tab->scroll_offset = 0;
    tab->input_len = 0;
    tab->cursor_pos = 0;
    tab->history_pos = -1;
    tab->history_count = 0;

    /* Initialize tab shell context */
    shell_init(&tab->shell_ctx);

    if (initial_cwd) {
        snprintf(tab->cwd, sizeof(tab->cwd), "%s", initial_cwd);
        snprintf(tab->shell_ctx.cwd, sizeof(tab->shell_ctx.cwd), "%s", initial_cwd);
    } else {
        if (!getcwd(tab->cwd, sizeof(tab->cwd))) {
            strncpy(tab->cwd, "/", sizeof(tab->cwd) - 1);
        }
        snprintf(tab->shell_ctx.cwd, sizeof(tab->shell_ctx.cwd), "%s", tab->cwd);
    }

    int new_idx = app->num_tabs;
    app->tabs[new_idx] = tab;
    app->num_tabs++;
    app->active_tab_idx = new_idx;

    return new_idx;
}

/**
 * @brief Closes an existing tab by index and frees its memory.
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in] tab_idx Index of tab to close.
 */
void gui_close_tab(GuiApp *app, int tab_idx) {
    if (!app || tab_idx < 0 || tab_idx >= app->num_tabs) {
        return;
    }

    Tab *tab = app->tabs[tab_idx];
    if (tab->out_pipe_fd >= 0) close(tab->out_pipe_fd);
    if (tab->running_pid > 0) kill(tab->running_pid, SIGTERM);
    free(tab);

    /* Shift remaining tabs in array left */
    for (int i = tab_idx; i < app->num_tabs - 1; i++) {
        app->tabs[i] = app->tabs[i + 1];
    }
    app->tabs[app->num_tabs - 1] = NULL;
    app->num_tabs--;

    /* If all tabs closed, exit program */
    if (app->num_tabs == 0) {
        app->is_running = 0;
        return;
    }

    /* Adjust active tab index */
    if (app->active_tab_idx >= app->num_tabs) {
        app->active_tab_idx = app->num_tabs - 1;
    }
}

/**
 * @brief Switches the active view to another tab.
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in] tab_idx Index of the target tab.
 */
void gui_switch_tab(GuiApp *app, int tab_idx) {
    if (!app || tab_idx < 0 || tab_idx >= app->num_tabs) {
        return;
    }
    app->active_tab_idx = tab_idx;
}

/**
 * @brief Appends raw incoming character bytes from command output into the tab's buffer.
 *
 * Handles newlines ('\n') and line wrapping so text stays formatted cleanly.
 *
 * @param[in,out] tab Pointer to target Tab.
 * @param[in] text Character buffer read from child pipe.
 * @param[in] len Number of bytes read.
 */
void tab_append_output(Tab *tab, const char *text, int len) {
    if (!tab || !text || len <= 0) {
        return;
    }

    /* Auto-snap view to bottom when new command output arrives */
    tab->scroll_offset = 0;

    /* Ensure output begins on the line below the command prompt, never overwriting it */
    if (tab->num_lines > 0 && tab->lines[tab->num_lines - 1].type == LINE_TYPE_PROMPT_INPUT) {
        if (tab->num_lines < MAX_BUFFER_LINES) {
            tab->num_lines++;
        } else {
            memmove(&tab->lines[0], &tab->lines[1], sizeof(TermLine) * (MAX_BUFFER_LINES - 1));
        }
        TermLine *first_out = &tab->lines[tab->num_lines - 1];
        memset(first_out, 0, sizeof(TermLine));
        first_out->type = LINE_TYPE_OUTPUT;
    }

    for (int i = 0; i < len; i++) {
        char c = text[i];

        if (c == '\r') {
            continue; /* Ignore carriage returns */
        }

        if (c == '\n') {
            /* Start a new output line */
            if (tab->num_lines < MAX_BUFFER_LINES) {
                tab->num_lines++;
            } else {
                /* Scroll buffer up by one line */
                memmove(&tab->lines[0], &tab->lines[1], sizeof(TermLine) * (MAX_BUFFER_LINES - 1));
            }
            TermLine *cur = &tab->lines[tab->num_lines - 1];
            memset(cur, 0, sizeof(TermLine));
            cur->type = LINE_TYPE_OUTPUT;
            continue;
        }

        /* If we have no lines yet, initialize line 0 */
        if (tab->num_lines == 0) {
            tab->num_lines = 1;
            memset(&tab->lines[0], 0, sizeof(TermLine));
            tab->lines[0].type = LINE_TYPE_OUTPUT;
        }

        TermLine *cur = &tab->lines[tab->num_lines - 1];
        size_t cur_len = strlen(cur->text);

        /* Wrap line if exceeding buffer line capacity */
        if (cur_len >= MAX_LINE_CHARS - 2) {
            if (tab->num_lines < MAX_BUFFER_LINES) {
                tab->num_lines++;
            } else {
                memmove(&tab->lines[0], &tab->lines[1], sizeof(TermLine) * (MAX_BUFFER_LINES - 1));
            }
            cur = &tab->lines[tab->num_lines - 1];
            memset(cur, 0, sizeof(TermLine));
            cur->type = LINE_TYPE_OUTPUT;
            cur_len = 0;
        }

        cur->text[cur_len] = c;
        cur->text[cur_len + 1] = '\0';
    }
}

/**
 * @brief Submits the current typed input in the tab for asynchronous execution.
 *
 * Creates a pipe to capture stdout/stderr from the command, forks the execution child,
 * records the input into the scrollback buffer with prompt styling, and clears the input line.
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in,out] tab Pointer to Tab where Enter was pressed.
 */
void tab_submit_command(GuiApp *app, Tab *tab) {
    if (!app || !tab) {
        return;
    }

    /* Snap view to bottom on submission */
    tab->scroll_offset = 0;

    /* Select active prompt prefix: primary "user@terminator> " or continuation "> " */
    const char *prompt_used = tab->is_multiline ? "> " : PROMPT_STRING;
    int p_len = (int)strlen(prompt_used);

    /* Record prompt + typed input into tab scrollback buffer with visual line wrapping */
    int text_area_w = app->win_width - 25;
    int cols = text_area_w / app->font_width;
    if (cols < 20) cols = 20;
    int first_line_capacity = cols - p_len;
    if (first_line_capacity < 1) first_line_capacity = 1;

    if (tab->input_len <= first_line_capacity) {
        if (tab->num_lines < MAX_BUFFER_LINES) {
            tab->num_lines++;
        } else {
            memmove(&tab->lines[0], &tab->lines[1], sizeof(TermLine) * (MAX_BUFFER_LINES - 1));
        }
        TermLine *prompt_line = &tab->lines[tab->num_lines - 1];
        prompt_line->type = LINE_TYPE_PROMPT_INPUT;
        prompt_line->prompt_len = p_len;
        snprintf(prompt_line->text, sizeof(prompt_line->text), "%s%.*s",
                 prompt_used, (int)(sizeof(prompt_line->text) - p_len - 1), tab->input_buffer);
    } else {
        /* Record Row 0 with prompt */
        if (tab->num_lines < MAX_BUFFER_LINES) {
            tab->num_lines++;
        } else {
            memmove(&tab->lines[0], &tab->lines[1], sizeof(TermLine) * (MAX_BUFFER_LINES - 1));
        }
        TermLine *first_line = &tab->lines[tab->num_lines - 1];
        first_line->type = LINE_TYPE_PROMPT_INPUT;
        first_line->prompt_len = p_len;
        snprintf(first_line->text, sizeof(first_line->text), "%s%.*s",
                 prompt_used, first_line_capacity, tab->input_buffer);

        /* Record subsequent wrapped rows cleanly */
        int remaining = tab->input_len - first_line_capacity;
        int buf_offset = first_line_capacity;
        while (remaining > 0) {
            int chunk = remaining > cols ? cols : remaining;
            if (tab->num_lines < MAX_BUFFER_LINES) {
                tab->num_lines++;
            } else {
                memmove(&tab->lines[0], &tab->lines[1], sizeof(TermLine) * (MAX_BUFFER_LINES - 1));
            }
            TermLine *wrap_line = &tab->lines[tab->num_lines - 1];
            wrap_line->type = LINE_TYPE_PROMPT_INPUT;
            wrap_line->prompt_len = 0;
            snprintf(wrap_line->text, sizeof(wrap_line->text), "%.*s", chunk, tab->input_buffer + buf_offset);
            buf_offset += chunk;
            remaining -= chunk;
        }
    }

    /* If user pressed Enter on an empty line and not in multiline, nothing to execute */
    if (tab->input_len == 0 && !tab->is_multiline) {
        return;
    }

    /* Accumulate into multiline buffer */
    if (!tab->is_multiline) {
        snprintf(tab->multiline_accum, sizeof(tab->multiline_accum), "%s", tab->input_buffer);
    } else {
        size_t acc_len = strlen(tab->multiline_accum);
        snprintf(tab->multiline_accum + acc_len, sizeof(tab->multiline_accum) - acc_len, "\n%s", tab->input_buffer);
    }

    /* Record command into history (avoiding consecutive duplicates) */
    if (tab->input_len > 0) {
        if (tab->history_count == 0 || strcmp(tab->history[tab->history_count - 1], tab->input_buffer) != 0) {
            if (tab->history_count < MAX_TAB_HISTORY) {
                snprintf(tab->history[tab->history_count], sizeof(tab->history[0]), "%s", tab->input_buffer);
                tab->history_count++;
            } else {
                memmove(&tab->history[0], &tab->history[1], sizeof(tab->history[0]) * (MAX_TAB_HISTORY - 1));
                snprintf(tab->history[MAX_TAB_HISTORY - 1], sizeof(tab->history[0]), "%s", tab->input_buffer);
            }
        }
        tab->history_pos = -1;
        tab->saved_input[0] = '\0';
    }

    /* Clear the active typed input buffer */
    tab->input_buffer[0] = '\0';
    tab->input_len = 0;
    tab->cursor_pos = 0;

    /* Check if the command is incomplete (unclosed quotes, trailing '\', etc.) */
    if (is_command_incomplete(tab->multiline_accum)) {
        tab->is_multiline = 1;
        return; /* Keep reading with continuation prompt '> ' */
    }

    /* Command is complete; reset multiline flag and copy accumulated command */
    tab->is_multiline = 0;
    char cmd_str[MAX_LINE_CHARS];
    strncpy(cmd_str, tab->multiline_accum, sizeof(cmd_str) - 1);
    cmd_str[sizeof(cmd_str) - 1] = '\0';
    tab->multiline_accum[0] = '\0';

    /* Parse command into pipeline */
    Pipeline pipeline;
    if (parse_line(cmd_str, &tab->shell_ctx, &pipeline) != 0) {
        tab_append_output(tab, "terminator: syntax error\n", 25);
        return;
    }

    /* Check for built-ins that only modify shell state and don't need piping (e.g. exit, cd) */
    if (pipeline.num_commands == 1 && is_builtin_command(pipeline.commands[0].argv[0])) {
        Command *cmd = &pipeline.commands[0];
        if (strcmp(cmd->argv[0], "exit") == 0) {
            free_pipeline(&pipeline);
            /* Close tab on exit */
            gui_close_tab(app, app->active_tab_idx);
            return;
        } else if (strcmp(cmd->argv[0], "cd") == 0) {
            /* Execute cd directly in tab context so cwd changes */
            int status = execute_builtin(&tab->shell_ctx, cmd);
            if (status != 0) {
                char err_buf[256];
                snprintf(err_buf, sizeof(err_buf), "terminator: cd: %s: No such file or directory\n",
                         cmd->argv[1] ? cmd->argv[1] : "");
                tab_append_output(tab, err_buf, strlen(err_buf));
            } else {
                snprintf(tab->cwd, sizeof(tab->cwd), "%s", tab->shell_ctx.cwd);
            }
            free_pipeline(&pipeline);
            return;
        } else if (strcmp(cmd->argv[0], "clear") == 0) {
            /* Clear visual scrollback buffer and reset vertical scroll position */
            tab->num_lines = 0;
            tab->scroll_offset = 0;
            free_pipeline(&pipeline);
            gui_redraw(app);
            return;
        }
    }

    /* Asynchronous command execution using a pipe:
     * 1. pipe() creates a channel [0]=read, [1]=write.
     * 2. fork() child: redirects stdout & stderr to write-end using dup2().
     * 3. Parent: sets read-end to non-blocking and registers it with poll() loop.
     */
    int pipefds[2];
    if (pipe(pipefds) < 0) {
        perror("terminator: pipe failed");
        free_pipeline(&pipeline);
        return;
    }

    /* Make read-end non-blocking so read() won't hang the GUI */
    int flags = fcntl(pipefds[0], F_GETFL, 0);
    fcntl(pipefds[0], F_SETFL, flags | O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0) {
        perror("terminator: fork failed");
        close(pipefds[0]);
        close(pipefds[1]);
        free_pipeline(&pipeline);
        return;
    }

    if (pid == 0) {
        /* Child process: change to tab's current working directory */
        if (chdir(tab->cwd) != 0) {
            /* If cwd changed or deleted, ignore */
        }

        /* Redirect stdout and stderr to the write end of the pipe */
        dup2(pipefds[1], STDOUT_FILENO);
        dup2(pipefds[1], STDERR_FILENO);
        close(pipefds[0]);
        close(pipefds[1]);

        /* Execute the parsed pipeline */
        int ret = shell_execute_pipeline(&tab->shell_ctx, &pipeline);
        _exit(ret);
    }

    /* Parent process: close write-end, store read-end for poll() loop */
    close(pipefds[1]);
    tab->out_pipe_fd = pipefds[0];
    tab->running_pid = pid;
    tab->is_running_cmd = 1;

    free_pipeline(&pipeline);
}

/**
 * @brief Handles keyboard input: typing, Backspace, Enter, arrows, and tab management shortcuts.
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in] event Pointer to XKeyEvent.
 */
void gui_handle_key_press(GuiApp *app, XKeyEvent *event) {
    if (!app || app->num_tabs == 0) {
        return;
    }
    Tab *tab = app->tabs[app->active_tab_idx];

    KeySym keysym;
    char buffer[32];
    int count = XLookupString(event, buffer, sizeof(buffer) - 1, &keysym, NULL);
    buffer[count] = '\0';

    int is_ctrl = (event->state & ControlMask);
    int is_shift = (event->state & ShiftMask);

    /* Keyboard shortcut: Ctrl+Shift+T creates a new tab */
    if (is_ctrl && is_shift && (keysym == XK_T || keysym == XK_t)) {
        gui_create_tab(app, tab->cwd);
        gui_redraw(app);
        return;
    }

    /* Keyboard shortcut: Ctrl+Shift+W closes the active tab */
    if (is_ctrl && is_shift && (keysym == XK_W || keysym == XK_w)) {
        gui_close_tab(app, app->active_tab_idx);
        gui_redraw(app);
        return;
    }

    /* Keyboard shortcut: Shift+Left / Shift+Right to cycle through tabs */
    if (is_shift && keysym == XK_Left) {
        if (app->active_tab_idx > 0) {
            gui_switch_tab(app, app->active_tab_idx - 1);
            gui_redraw(app);
        }
        return;
    }
    if (is_shift && keysym == XK_Right) {
        if (app->active_tab_idx < app->num_tabs - 1) {
            gui_switch_tab(app, app->active_tab_idx + 1);
            gui_redraw(app);
        }
        return;
    }

    /* Handle Home / End keys for rapid line navigation */
    if (keysym == XK_Home) {
        tab->cursor_pos = 0;
        gui_redraw(app);
        return;
    }
    if (keysym == XK_End) {
        tab->cursor_pos = tab->input_len;
        gui_redraw(app);
        return;
    }

    /* Handle Return / Enter key */
    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        tab_submit_command(app, tab);
        gui_redraw(app);
        return;
    }

    /* Handle Backspace key */
    if (keysym == XK_BackSpace) {
        if (tab->cursor_pos > 0) {
            memmove(&tab->input_buffer[tab->cursor_pos - 1],
                    &tab->input_buffer[tab->cursor_pos],
                    tab->input_len - tab->cursor_pos + 1);
            tab->cursor_pos--;
            tab->input_len--;
            gui_redraw(app);
        }
        return;
    }

    /* Handle Left / Right arrow navigation within the input buffer */
    if (keysym == XK_Left && !is_ctrl && !is_shift) {
        if (tab->cursor_pos > 0) {
            tab->cursor_pos--;
            gui_redraw(app);
        }
        return;
    }
    if (keysym == XK_Right && !is_ctrl && !is_shift) {
        if (tab->cursor_pos < tab->input_len) {
            tab->cursor_pos++;
            gui_redraw(app);
        }
        return;
    }

    /* Check Task 7 configurable hotkeys table (Ctrl+A, Ctrl+E, Ctrl+U, Ctrl+K, Ctrl+W, Ctrl+L, Ctrl+D, Ctrl+Shift+C, Ctrl+Shift+V, Up, Down) */
    int text_area_w = app->win_width - 25;
    int cols = text_area_w / app->font_width;
    if (cols < 20) cols = 20;
    const char *active_prompt = tab->is_multiline ? "> " : PROMPT_STRING;
    int prompt_len = (int)strlen(active_prompt);
    int first_line_capacity = cols - prompt_len;
    if (first_line_capacity < 1) first_line_capacity = 1;

    int action = hotkeys_match(&app->hotkeys, event, keysym);
    if (action >= 0) {
        switch (action) {
            case HOTKEY_MOVE_START:
                tab->cursor_pos = 0;
                gui_redraw(app);
                return;

            case HOTKEY_MOVE_END:
                tab->cursor_pos = tab->input_len;
                gui_redraw(app);
                return;

            case HOTKEY_DELETE_TO_START:
                if (tab->cursor_pos > 0) {
                    memmove(&tab->input_buffer[0],
                            &tab->input_buffer[tab->cursor_pos],
                            tab->input_len - tab->cursor_pos + 1);
                    tab->input_len -= tab->cursor_pos;
                    tab->cursor_pos = 0;
                    gui_redraw(app);
                }
                return;

            case HOTKEY_DELETE_TO_END:
                if (tab->cursor_pos < tab->input_len) {
                    tab->input_buffer[tab->cursor_pos] = '\0';
                    tab->input_len = tab->cursor_pos;
                    gui_redraw(app);
                }
                return;

            case HOTKEY_DELETE_WORD:
                if (tab->cursor_pos > 0) {
                    int end = tab->cursor_pos;
                    int start = end;
                    while (start > 0 && isspace((unsigned char)tab->input_buffer[start - 1])) {
                        start--;
                    }
                    while (start > 0 && !isspace((unsigned char)tab->input_buffer[start - 1])) {
                        start--;
                    }
                    int del_len = end - start;
                    memmove(&tab->input_buffer[start],
                            &tab->input_buffer[end],
                            tab->input_len - end + 1);
                    tab->input_len -= del_len;
                    tab->cursor_pos = start;
                    gui_redraw(app);
                }
                return;

            case HOTKEY_CLEAR_SCREEN:
                tab->num_lines = 0;
                tab->scroll_offset = 0;
                gui_redraw(app);
                return;

            case HOTKEY_EXIT_SHELL:
                if (tab->input_len == 0) {
                    gui_close_tab(app, app->active_tab_idx);
                    gui_redraw(app);
                    return;
                }
                break;

            case HOTKEY_COPY:
                if (tab->input_len > 0) {
                    snprintf(app->clipboard_data, sizeof(app->clipboard_data), "%s", tab->input_buffer);
                    app->clipboard_len = (int)strlen(app->clipboard_data);
                    XSetSelectionOwner(app->display, app->atom_clipboard, app->window, CurrentTime);
                    XSetSelectionOwner(app->display, XA_PRIMARY, app->window, CurrentTime);
                }
                return;

            case HOTKEY_PASTE:
                XConvertSelection(app->display, app->atom_clipboard, app->atom_utf8_string,
                                  app->atom_terminator_sel, app->window, CurrentTime);
                return;

            case HOTKEY_HISTORY_PREV:
                /* If command is multi-line wrapped and cursor is on line 2+, navigate up inside command */
                if (tab->input_len > first_line_capacity && tab->cursor_pos > first_line_capacity) {
                    int rel = tab->cursor_pos - first_line_capacity;
                    int row = 1 + (rel / cols);
                    if (row == 1) {
                        int col = rel % cols;
                        int target = col - prompt_len;
                        if (target < 0) target = 0;
                        if (target > first_line_capacity) target = first_line_capacity;
                        tab->cursor_pos = target;
                    } else {
                        tab->cursor_pos -= cols;
                    }
                    gui_redraw(app);
                    return;
                }
                /* Otherwise browse command history backwards */
                if (tab->history_count > 0) {
                    if (tab->history_pos == -1) {
                        snprintf(tab->saved_input, sizeof(tab->saved_input), "%s", tab->input_buffer);
                        tab->history_pos = tab->history_count - 1;
                    } else if (tab->history_pos > 0) {
                        tab->history_pos--;
                    }
                    snprintf(tab->input_buffer, sizeof(tab->input_buffer), "%s", tab->history[tab->history_pos]);
                    tab->input_len = (int)strlen(tab->input_buffer);
                    tab->cursor_pos = tab->input_len;
                    gui_redraw(app);
                    return;
                }
                break;

            case HOTKEY_HISTORY_NEXT:
                /* If command is multi-line wrapped and cursor is not on last line, navigate down inside command */
                if (tab->input_len > first_line_capacity) {
                    int total_rows = 1 + (tab->input_len - first_line_capacity + cols - 1) / cols;
                    int cur_row = (tab->cursor_pos <= first_line_capacity) ? 0 : 1 + ((tab->cursor_pos - first_line_capacity) / cols);
                    if (cur_row < total_rows - 1) {
                        int target;
                        if (cur_row == 0) target = first_line_capacity + tab->cursor_pos;
                        else target = tab->cursor_pos + cols;
                        if (target > tab->input_len) target = tab->input_len;
                        tab->cursor_pos = target;
                        gui_redraw(app);
                        return;
                    }
                }
                /* Otherwise browse command history forwards */
                if (tab->history_pos >= 0) {
                    if (tab->history_pos < tab->history_count - 1) {
                        tab->history_pos++;
                        snprintf(tab->input_buffer, sizeof(tab->input_buffer), "%s", tab->history[tab->history_pos]);
                        tab->input_len = (int)strlen(tab->input_buffer);
                        tab->cursor_pos = tab->input_len;
                    } else {
                        /* Restores active draft */
                        tab->history_pos = -1;
                        snprintf(tab->input_buffer, sizeof(tab->input_buffer), "%s", tab->saved_input);
                        tab->input_len = (int)strlen(tab->input_buffer);
                        tab->cursor_pos = tab->input_len;
                    }
                    gui_redraw(app);
                    return;
                }
                break;

            default:
                break;
        }
    }

    /* Keyboard scrolling: Page_Up / Page_Down or Shift+Up / Shift+Down */
    int content_top = TAB_BAR_HEIGHT + 10;
    int visible_rows = (app->win_height - content_top - app->font_height) / app->font_height;
    if (visible_rows <= 0) visible_rows = 1;
    int max_scroll = tab->num_lines > visible_rows ? (tab->num_lines - visible_rows) : 0;

    if (keysym == XK_Page_Up || (is_shift && keysym == XK_Up)) {
        int step = (keysym == XK_Page_Up) ? (visible_rows / 2 > 0 ? visible_rows / 2 : 1) : 1;
        tab->scroll_offset += step;
        if (tab->scroll_offset > max_scroll) {
            tab->scroll_offset = max_scroll;
        }
        gui_redraw(app);
        return;
    }
    if (keysym == XK_Page_Down || (is_shift && keysym == XK_Down)) {
        int step = (keysym == XK_Page_Down) ? (visible_rows / 2 > 0 ? visible_rows / 2 : 1) : 1;
        tab->scroll_offset -= step;
        if (tab->scroll_offset < 0) {
            tab->scroll_offset = 0;
        }
        gui_redraw(app);
        return;
    }

    /* Handle Return / Enter key */
    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        tab_submit_command(app, tab);
        gui_redraw(app);
        return;
    }

    /* Handle Backspace key */
    if (keysym == XK_BackSpace) {
        if (tab->cursor_pos > 0) {
            memmove(&tab->input_buffer[tab->cursor_pos - 1],
                    &tab->input_buffer[tab->cursor_pos],
                    tab->input_len - tab->cursor_pos + 1);
            tab->cursor_pos--;
            tab->input_len--;
            gui_redraw(app);
        }
        return;
    }

    /* Handle Left / Right arrow navigation within the input buffer */
    if (keysym == XK_Left) {
        if (tab->cursor_pos > 0) {
            tab->cursor_pos--;
            gui_redraw(app);
        }
        return;
    }
    if (keysym == XK_Right) {
        if (tab->cursor_pos < tab->input_len) {
            tab->cursor_pos++;
            gui_redraw(app);
        }
        return;
    }

    /* Handle printable character input */
    if (count > 0 && buffer[0] >= 32 && buffer[0] <= 126) {
        if (tab->input_len < MAX_LINE_CHARS - 2) {
            memmove(&tab->input_buffer[tab->cursor_pos + 1],
                    &tab->input_buffer[tab->cursor_pos],
                    tab->input_len - tab->cursor_pos + 1);
            tab->input_buffer[tab->cursor_pos] = buffer[0];
            tab->cursor_pos++;
            tab->input_len++;
            gui_redraw(app);
        }
        return;
    }
}

/**
 * @brief Handles mouse clicks on the tab bar: tab selection, '+' button to add tabs.
 *
 * @param[in,out] app Pointer to GuiApp.
 * @param[in] event Pointer to XButtonEvent.
 */
void gui_handle_button_press(GuiApp *app, XButtonEvent *event) {
    if (!app || app->num_tabs == 0) {
        return;
    }
    Tab *tab = app->tabs[app->active_tab_idx];

    /* Mouse wheel scrolling: Button4 = Wheel Up, Button5 = Wheel Down */
    int content_top = TAB_BAR_HEIGHT + 10;
    int visible_rows = (app->win_height - content_top - app->font_height) / app->font_height;
    if (visible_rows <= 0) visible_rows = 1;
    int max_scroll = tab->num_lines > visible_rows ? (tab->num_lines - visible_rows) : 0;

    if (event->button == 4) {
        /* Wheel up -> scroll into past history */
        tab->scroll_offset += 3;
        if (tab->scroll_offset > max_scroll) {
            tab->scroll_offset = max_scroll;
        }
        gui_redraw(app);
        return;
    }
    if (event->button == 5) {
        /* Wheel down -> scroll down towards active prompt */
        tab->scroll_offset -= 3;
        if (tab->scroll_offset < 0) {
            tab->scroll_offset = 0;
        }
        gui_redraw(app);
        return;
    }

    if (event->y > TAB_BAR_HEIGHT) {
        return; /* Regular click outside tab bar */
    }

    /* Pinned '+' new tab button at top-right corner */
    const char *plus_label = " [ + ] ";
    int plus_width = (int)strlen(plus_label) * app->font_width + 8;
    int plus_x = app->win_width - plus_width - 6;

    if (event->x >= plus_x && event->x <= plus_x + plus_width) {
        gui_create_tab(app, app->tabs[app->active_tab_idx]->cwd);
        gui_redraw(app);
        return;
    }

    /* Compute tab positions matching gui_redraw */
    int avail_tabs_width = plus_x - 14;
    int tab_spacing = 4;
    int tab_width = 180;
    if (app->num_tabs > 0) {
        int max_allowed_w = (avail_tabs_width - ((app->num_tabs - 1) * tab_spacing)) / app->num_tabs;
        if (max_allowed_w < tab_width) {
            tab_width = max_allowed_w;
        }
        if (tab_width < 45) {
            tab_width = 45;
        }
    }

    int current_x = 10;
    for (int i = 0; i < app->num_tabs; i++) {
        if (event->x >= current_x && event->x < current_x + tab_width) {
            gui_switch_tab(app, i);
            gui_redraw(app);
            return;
        }
        current_x += tab_width + tab_spacing;
    }
}

/**
 * @brief Redraws the terminal window:
 *        1. Tab bar at the top with tabs and '+' button.
 *        2. Terminal scrollback lines.
 *        3. Red prompt string ("user@terminator> ").
 *        4. Neon green typed user command and cursor block.
 *
 * Uses double-buffering via back_buffer to prevent screen flickering.
 *
 * @param[in,out] app Pointer to GuiApp.
 */
void gui_redraw(GuiApp *app) {
    if (!app || !app->display || app->num_tabs == 0) {
        return;
    }

    Tab *tab = app->tabs[app->active_tab_idx];

    /* 1. Clear off-screen back buffer to pitch black */
    XFillRectangle(app->display, app->back_buffer, app->gc_bg,
                   0, 0, app->win_width, app->win_height);

    /* 2. Draw Tab Bar strip at top */
    XFillRectangle(app->display, app->back_buffer, app->gc_tab_bar_bg,
                   0, 0, app->win_width, TAB_BAR_HEIGHT);

    /* Render docked '[ + ]' button pinned at the far-right corner */
    const char *plus_label = " [ + ] ";
    int plus_width = (int)strlen(plus_label) * app->font_width + 8;
    int plus_x = app->win_width - plus_width - 6;
    int tab_text_y = TAB_BAR_HEIGHT - 8;

    /* Draw distinct clickable background box for the '+' button */
    XFillRectangle(app->display, app->back_buffer, app->gc_tab_active_bg,
                   plus_x, 3, plus_width, TAB_BAR_HEIGHT - 6);
    XDrawString(app->display, app->back_buffer, app->gc_tab_active_text,
                plus_x + 4, tab_text_y, plus_label, (int)strlen(plus_label));

    /* Compute available horizontal space for tabs */
    int avail_tabs_width = plus_x - 14;
    int tab_spacing = 4;
    int tab_width = 180;
    if (app->num_tabs > 0) {
        int max_allowed_w = (avail_tabs_width - ((app->num_tabs - 1) * tab_spacing)) / app->num_tabs;
        if (max_allowed_w < tab_width) {
            tab_width = max_allowed_w;
        }
        if (tab_width < 45) {
            tab_width = 45;
        }
    }

    int cur_x = 10;
    for (int i = 0; i < app->num_tabs; i++) {
        char label[128];
        const char *cwd = app->tabs[i]->cwd;
        const char *last_slash = strrchr(cwd, '/');
        const char *short_name = last_slash ? (last_slash + 1) : cwd;
        if (!short_name || short_name[0] == '\0') short_name = "/";

        /* Determine how many characters fit into the tab width */
        int max_label_chars = (tab_width - 10) / app->font_width;
        if (max_label_chars <= 4) {
            snprintf(label, sizeof(label), " %d ", i + 1);
        } else {
            /* Format label with truncation if name is too long */
            int name_chars_avail = max_label_chars - 7;
            if (name_chars_avail > 0 && (int)strlen(short_name) > name_chars_avail) {
                snprintf(label, sizeof(label), " [ %d: %.*s.. ] ", i + 1,
                         name_chars_avail > 2 ? name_chars_avail - 2 : 1, short_name);
            } else {
                snprintf(label, sizeof(label), " [ %d: %.30s ] ", i + 1, short_name);
            }
        }
        int label_len = (int)strlen(label);

        if (i == app->active_tab_idx) {
            /* Highlight active tab */
            XFillRectangle(app->display, app->back_buffer, app->gc_tab_active_bg,
                           cur_x, 3, tab_width, TAB_BAR_HEIGHT - 6);
            XDrawString(app->display, app->back_buffer, app->gc_tab_active_text,
                        cur_x + 5, tab_text_y, label, label_len);
        } else {
            XDrawString(app->display, app->back_buffer, app->gc_tab_text,
                        cur_x + 5, tab_text_y, label, label_len);
        }

        cur_x += tab_width + tab_spacing;
    }

    /* 3. Calculate terminal text area dimensions & visual line wrapping */
    int text_area_w = app->win_width - 25;
    int cols = text_area_w / app->font_width;
    if (cols < 20) cols = 20;

    const char *active_prompt = tab->is_multiline ? "> " : PROMPT_STRING;
    int prompt_len = (int)strlen(active_prompt);
    int first_line_capacity = cols - prompt_len;
    if (first_line_capacity < 1) first_line_capacity = 1;

    /* Calculate how many visual rows the active wrapped input will occupy */
    int active_input_rows = 1;
    if (tab->input_len > first_line_capacity) {
        active_input_rows = 1 + (tab->input_len - first_line_capacity + cols - 1) / cols;
    }

    int content_top = TAB_BAR_HEIGHT + 10;
    int available_content_height = app->win_height - content_top - (active_input_rows * app->font_height);
    int visible_rows = available_content_height / app->font_height;
    if (visible_rows <= 0) visible_rows = 1;

    /* Calculate scroll limits and determine starting/ending lines to render */
    int max_scroll = tab->num_lines > visible_rows ? (tab->num_lines - visible_rows) : 0;
    if (tab->scroll_offset > max_scroll) tab->scroll_offset = max_scroll;
    if (tab->scroll_offset < 0) tab->scroll_offset = 0;

    int start_line = max_scroll - tab->scroll_offset;
    int end_line = start_line + visible_rows;
    if (end_line > tab->num_lines) end_line = tab->num_lines;

    int current_y = content_top + app->font_ascent;

    /* Render scrollback lines within visible window */
    for (int i = start_line; i < end_line; i++) {
        TermLine *line = &tab->lines[i];

        if (line->type == LINE_TYPE_PROMPT_INPUT) {
            /* Render Prompt prefix in RED */
            int p_len = line->prompt_len;
            if (p_len > 0) {
                XDrawString(app->display, app->back_buffer, app->gc_prompt,
                            10, current_y, line->text, p_len);
            }
            /* Render the typed command in NEON GREEN */
            int cmd_len = (int)strlen(line->text) - p_len;
            if (cmd_len > 0) {
                XDrawString(app->display, app->back_buffer, app->gc_input,
                            10 + (p_len * app->font_width), current_y,
                            line->text + p_len, cmd_len);
            }
        } else {
            /* Render program output in crisp Light Text */
            XDrawString(app->display, app->back_buffer, app->gc_output,
                        10, current_y, line->text, (int)strlen(line->text));
        }

        current_y += app->font_height;
    }

    /* 4. Render Active Input Line with Visual Line Wrapping:
     * - Primary "user@terminator> " or continuation "> " in Vivid Red on Line 1.
     * - User's typed input drawn in Neon Hacker Green.
     * - If input exceeds line width, wraps cleanly onto subsequent visual lines.
     * - Block cursor rendered at exact wrapped (row, col) position.
     */
    XDrawString(app->display, app->back_buffer, app->gc_prompt,
                10, current_y, active_prompt, prompt_len);

    if (tab->input_len <= first_line_capacity) {
        if (tab->input_len > 0) {
            XDrawString(app->display, app->back_buffer, app->gc_input,
                        10 + (prompt_len * app->font_width), current_y,
                        tab->input_buffer, tab->input_len);
        }
        int cursor_x = 10 + ((prompt_len + tab->cursor_pos) * app->font_width);
        int cursor_y = current_y - app->font_ascent;
        XFillRectangle(app->display, app->back_buffer, app->gc_cursor,
                       cursor_x, cursor_y, app->font_width, app->font_height);
    } else {
        /* Line 0: draw first_line_capacity characters */
        XDrawString(app->display, app->back_buffer, app->gc_input,
                    10 + (prompt_len * app->font_width), current_y,
                    tab->input_buffer, first_line_capacity);

        /* Lines 1, 2, ...: draw remaining characters in wrapped lines */
        int remaining = tab->input_len - first_line_capacity;
        int buf_offset = first_line_capacity;
        int wrap_row = 1;
        while (remaining > 0) {
            int chunk = remaining > cols ? cols : remaining;
            int wrap_y = current_y + (wrap_row * app->font_height);
            XDrawString(app->display, app->back_buffer, app->gc_input,
                        10, wrap_y, tab->input_buffer + buf_offset, chunk);
            buf_offset += chunk;
            remaining -= chunk;
            wrap_row++;
        }

        /* Calculate cursor position across wrapped lines */
        int cur_row = 0;
        int cur_col = 0;
        if (tab->cursor_pos <= first_line_capacity) {
            cur_row = 0;
            cur_col = prompt_len + tab->cursor_pos;
        } else {
            int rel = tab->cursor_pos - first_line_capacity;
            cur_row = 1 + (rel / cols);
            cur_col = rel % cols;
        }
        int cursor_x = 10 + (cur_col * app->font_width);
        int cursor_y = current_y + (cur_row * app->font_height) - app->font_ascent;
        XFillRectangle(app->display, app->back_buffer, app->gc_cursor,
                       cursor_x, cursor_y, app->font_width, app->font_height);
    }

    /* 5. Draw right-side scrollbar thumb if buffer exceeds screen height */
    if (tab->num_lines > visible_rows) {
        int bar_x = app->win_width - 8;
        int bar_y_start = content_top;
        int bar_height_total = app->win_height - content_top - 5;
        int thumb_height = (visible_rows * bar_height_total) / tab->num_lines;
        if (thumb_height < 15) thumb_height = 15;
        int thumb_y = bar_y_start + (int)(((long)start_line * (bar_height_total - thumb_height)) / (tab->num_lines - visible_rows));
        XFillRectangle(app->display, app->back_buffer, app->gc_scrollbar,
                       bar_x, thumb_y, 5, thumb_height);
    }

    /* 6. Copy off-screen back buffer onto the visible X11 window in one shot */
    XCopyArea(app->display, app->back_buffer, app->window, app->gc_bg,
              0, 0, app->win_width, app->win_height, 0, 0);

    XFlush(app->display);
}

/**
 * @brief Main non-blocking event loop implementing poll() over X11 descriptor and pipes.
 *
 * Implements the architecture from slide 8:
 * - Sleeps via poll() when idle (0% CPU usage).
 * - Wakes up immediately when an X event arrives or when child command output arrives.
 * - Captures output from background tabs simultaneously without freezing the GUI.
 *
 * @param[in,out] app Pointer to GuiApp.
 */
void gui_run_loop(GuiApp *app) {
    if (!app || !app->display) {
        return;
    }

    int x_fd = ConnectionNumber(app->display);

    while (app->is_running) {
        /* Process all pending X11 events first */
        while (XPending(app->display) > 0) {
            XEvent ev;
            XNextEvent(app->display, &ev);

            switch (ev.type) {
                case Expose:
                    gui_redraw(app);
                    break;

                case ConfigureNotify:
                    /* Window resized */
                    if (ev.xconfigure.width != app->win_width ||
                        ev.xconfigure.height != app->win_height) {
                        app->win_width = ev.xconfigure.width;
                        app->win_height = ev.xconfigure.height;

                        /* Recreate back buffer matching new dimensions */
                        if (app->back_buffer) XFreePixmap(app->display, app->back_buffer);
                        app->back_buffer = XCreatePixmap(app->display, app->window,
                                                         app->win_width, app->win_height,
                                                         DefaultDepth(app->display, app->screen));
                        gui_redraw(app);
                    }
                    break;

                case KeyPress:
                    gui_handle_key_press(app, &ev.xkey);
                    break;

                case ButtonPress:
                    gui_handle_button_press(app, &ev.xbutton);
                    break;

                case ClientMessage:
                    /* Window close button clicked */
                    if ((Atom)ev.xclient.data.l[0] == app->wm_delete_window) {
                        app->is_running = 0;
                    }
                    break;

                case SelectionRequest: {
                    /* Reply to other applications or Terminator requesting clipboard text */
                    XSelectionRequestEvent *req = &ev.xselectionrequest;
                    XEvent reply;
                    memset(&reply, 0, sizeof(reply));
                    reply.xselection.type = SelectionNotify;
                    reply.xselection.display = req->display;
                    reply.xselection.requestor = req->requestor;
                    reply.xselection.selection = req->selection;
                    reply.xselection.target = req->target;
                    reply.xselection.time = req->time;
                    reply.xselection.property = None;

                    if (req->target == app->atom_targets) {
                        Atom targets[] = { app->atom_targets, app->atom_utf8_string, XA_STRING };
                        XChangeProperty(req->display, req->requestor, req->property,
                                        XA_ATOM, 32, PropModeReplace,
                                        (unsigned char *)targets, sizeof(targets) / sizeof(Atom));
                        reply.xselection.property = req->property;
                    } else if (req->target == XA_STRING || req->target == app->atom_utf8_string) {
                        XChangeProperty(req->display, req->requestor, req->property,
                                        req->target, 8, PropModeReplace,
                                        (unsigned char *)app->clipboard_data, app->clipboard_len);
                        reply.xselection.property = req->property;
                    }
                    XSendEvent(req->display, req->requestor, False, 0, &reply);
                    XFlush(app->display);
                    break;
                }

                case SelectionNotify: {
                    /* Receive clipboard text from paste request */
                    XSelectionEvent *sel = &ev.xselection;
                    if (sel->property != None) {
                        Atom type_ret;
                        int format_ret;
                        unsigned long nitems_ret, bytes_after_ret;
                        unsigned char *prop_data = NULL;
                        if (XGetWindowProperty(app->display, app->window, sel->property,
                                               0, 4096, True, AnyPropertyType,
                                               &type_ret, &format_ret, &nitems_ret,
                                               &bytes_after_ret, &prop_data) == Success && prop_data) {
                            if (app->num_tabs > 0) {
                                Tab *tab = app->tabs[app->active_tab_idx];
                                for (unsigned long i = 0; i < nitems_ret; i++) {
                                    char c = prop_data[i];
                                    if (c == '\r' || c == '\n') continue;
                                    if (c >= 32 && c <= 126 && tab->input_len < MAX_LINE_CHARS - 2) {
                                        memmove(&tab->input_buffer[tab->cursor_pos + 1],
                                                &tab->input_buffer[tab->cursor_pos],
                                                tab->input_len - tab->cursor_pos + 1);
                                        tab->input_buffer[tab->cursor_pos] = c;
                                        tab->cursor_pos++;
                                        tab->input_len++;
                                    }
                                }
                                gui_redraw(app);
                            }
                            XFree(prop_data);
                        }
                    }
                    break;
                }

                default:
                    break;
            }
        }

        /* Check if GUI was closed during event processing */
        if (!app->is_running) {
            break;
        }

        /* Set up pollfd array:
         * fds[0] = X11 connection descriptor (x_fd)
         * fds[1..N] = Read ends of active command pipes across all tabs
         */
        struct pollfd fds[1 + MAX_TABS];
        int num_poll_fds = 1;

        fds[0].fd = x_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        int tab_indices[1 + MAX_TABS];
        tab_indices[0] = -1;

        for (int i = 0; i < app->num_tabs; i++) {
            Tab *t = app->tabs[i];
            if (t && t->is_running_cmd && t->out_pipe_fd >= 0) {
                fds[num_poll_fds].fd = t->out_pipe_fd;
                fds[num_poll_fds].events = POLLIN | POLLHUP;
                fds[num_poll_fds].revents = 0;
                tab_indices[num_poll_fds] = i;
                num_poll_fds++;
            }
        }

        /* Sleep until an X11 event arrives OR child pipe data is ready (timeout 50ms) */
        int ret = poll(fds, num_poll_fds, 50);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("terminator: poll error");
            break;
        }

        int needs_redraw = 0;

        /* Check active child pipes for output */
        for (int i = 1; i < num_poll_fds; i++) {
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                int t_idx = tab_indices[i];
                Tab *t = app->tabs[t_idx];
                char buf[512];
                ssize_t n = read(t->out_pipe_fd, buf, sizeof(buf));

                if (n > 0) {
                    tab_append_output(t, buf, (int)n);
                    if (t_idx == app->active_tab_idx) {
                        needs_redraw = 1;
                    }
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    /* Pipe closed / child finished */
                    close(t->out_pipe_fd);
                    t->out_pipe_fd = -1;

                    int status = 0;
                    waitpid(t->running_pid, &status, WNOHANG);
                    if (WIFEXITED(status)) {
                        t->shell_ctx.last_exit_status = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        t->shell_ctx.last_exit_status = 128 + WTERMSIG(status);
                    }

                    t->is_running_cmd = 0;
                    t->running_pid = 0;

                    if (t_idx == app->active_tab_idx) {
                        needs_redraw = 1;
                    }
                }
            }
        }

        if (needs_redraw) {
            gui_redraw(app);
        }
    }
}
