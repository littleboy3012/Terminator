#include "gui.h"
#include "parser.h"
#include "builtins.h"
#include "history.h"
#include "completion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/wait.h>
#include <pthread.h>
#include <X11/Xatom.h>

#define PROMPT_STRING "user@terminator> "

/* ========================================================================= */
/*                         Static Helper Functions                           */
/* ========================================================================= */

static unsigned long allocate_color(Display *d, Colormap cm,
                                    unsigned short r, unsigned short g, unsigned short b) {
    XColor col;
    col.red = r;
    col.green = g;
    col.blue = b;
    col.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(d, cm, &col)) return col.pixel;
    return BlackPixel(d, DefaultScreen(d));
}

static GC create_gc(Display *d, Window w, unsigned long fg, XFontStruct *font) {
    XGCValues gcv;
    gcv.foreground = fg;
    if (font) gcv.font = font->fid;
    return XCreateGC(d, w, GCForeground | (font ? GCFont : 0), &gcv);
}

static int get_term_cols(const GuiApp *app) {
    int cols = (app->win_width - 25) / app->font_width;
    return cols < 20 ? 20 : cols;
}

static int get_first_line_capacity(const GuiApp *app, int prompt_len) {
    int cap = get_term_cols(app) - prompt_len;
    return cap < 1 ? 1 : cap;
}

static int get_max_scroll(const GuiApp *app, const Tab *tab) {
    int content_top = TAB_BAR_HEIGHT + 10;
    int visible_rows = (app->win_height - content_top - app->font_height) / app->font_height;
    if (visible_rows <= 0) visible_rows = 1;
    return tab->num_lines > visible_rows ? (tab->num_lines - visible_rows) : 0;
}

static void get_tab_bar_layout(const GuiApp *app, int *plus_x, int *plus_w, int *tab_w, int *spacing) {
    const char *plus_label = " [ + ] ";
    *plus_w = (int)strlen(plus_label) * app->font_width + 8;
    *plus_x = app->win_width - *plus_w - 6;
    *spacing = 4;
    *tab_w = 180;
    if (app->num_tabs > 0) {
        int max_w = (*plus_x - 14 - ((app->num_tabs - 1) * *spacing)) / app->num_tabs;
        if (max_w < *tab_w) *tab_w = max_w;
        if (*tab_w < 45) *tab_w = 45;
    }
}

static TermLine *tab_new_line(Tab *tab, LineType type, int prompt_len) {
    if (tab->num_lines < MAX_BUFFER_LINES) {
        tab->num_lines++;
    } else {
        memmove(&tab->lines[0], &tab->lines[1], sizeof(TermLine) * (MAX_BUFFER_LINES - 1));
    }
    TermLine *l = &tab->lines[tab->num_lines - 1];
    memset(l, 0, sizeof(TermLine));
    l->type = type;
    l->prompt_len = prompt_len;
    return l;
}

static void tab_record_prompt_input(GuiApp *app, Tab *tab, const char *prompt, int p_len) {
    int cols = get_term_cols(app);
    int first_cap = get_first_line_capacity(app, p_len);

    if (tab->input_len <= first_cap) {
        TermLine *l = tab_new_line(tab, LINE_TYPE_PROMPT_INPUT, p_len);
        snprintf(l->text, sizeof(l->text), "%s%.*s", prompt,
                 (int)(sizeof(l->text) - p_len - 1), tab->input_buffer);
    } else {
        TermLine *l0 = tab_new_line(tab, LINE_TYPE_PROMPT_INPUT, p_len);
        snprintf(l0->text, sizeof(l0->text), "%s%.*s", prompt, first_cap, tab->input_buffer);

        for (int off = first_cap; off < tab->input_len; off += cols) {
            int chunk = tab->input_len - off > cols ? cols : tab->input_len - off;
            TermLine *wl = tab_new_line(tab, LINE_TYPE_PROMPT_INPUT, 0);
            snprintf(wl->text, sizeof(wl->text), "%.*s", chunk, tab->input_buffer + off);
        }
    }
}

static void tab_insert_char(Tab *tab, char c) {
    if (tab->input_len < MAX_LINE_CHARS - 2) {
        memmove(&tab->input_buffer[tab->cursor_pos + 1],
                &tab->input_buffer[tab->cursor_pos],
                tab->input_len - tab->cursor_pos + 1);
        tab->input_buffer[tab->cursor_pos] = c;
        tab->cursor_pos++;
        tab->input_len++;
    }
}

static void tab_delete_range(Tab *tab, int start, int end) {
    if (start < 0) start = 0;
    if (end > tab->input_len) end = tab->input_len;
    if (start >= end) return;
    int len = end - start;
    memmove(&tab->input_buffer[start],
            &tab->input_buffer[end],
            tab->input_len - end + 1);
    tab->input_len -= len;
    tab->cursor_pos = start;
}

/* ========================================================================= */
/*                         Core GUI Lifecycle                                */
/* ========================================================================= */

static void *tab_worker_thread(void *arg) {
    Tab *tab = (Tab *)arg;
    while (tab && tab->is_thread_alive) {
        usleep(100000); /* 100ms idle heartbeat; tab thread lives for tab duration */
    }
    return NULL;
}

static void tab_process_main(Tab *tab, int in_fd, int out_fd) {
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    /* Redirect STDOUT and STDERR to out_fd so that all output goes to GUI */
    dup2(out_fd, STDOUT_FILENO);
    dup2(out_fd, STDERR_FILENO);
    if (out_fd != STDOUT_FILENO && out_fd != STDERR_FILENO) {
        close(out_fd);
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Close in_fd on exec so child processes do not inherit the command pipe */
    fcntl(in_fd, F_SETFD, FD_CLOEXEC);

    char cmd_line[MAX_LINE_CHARS * 4];
    FILE *in_fp = fdopen(in_fd, "r");
    if (!in_fp) _exit(1);

    while (1) {
        /* Check status of any background jobs before next command */
        char notif[1024];
        notif[0] = '\0';
        jobs_check_status(&tab->shell_ctx.job_table, notif, sizeof(notif));
        if (notif[0] != '\0') {
            printf("%s", notif);
            fflush(stdout);
        }

        if (fgets(cmd_line, sizeof(cmd_line), in_fp) == NULL) {
            if (errno == EINTR) {
                clearerr(in_fp);
                continue;
            }
            break;
        }

        size_t len = strlen(cmd_line);
        while (len > 0 && (cmd_line[len - 1] == '\n' || cmd_line[len - 1] == '\r')) {
            cmd_line[--len] = '\0';
        }
        if (len == 0) continue;

        if (strcmp(cmd_line, "exit") == 0 || tab->shell_ctx.should_exit) {
            break;
        }

        Pipeline pipeline;
        if (parse_line(cmd_line, &tab->shell_ctx, &pipeline) != 0) {
            const char *syn_err = "terminator: syntax error\n";
            if (write(STDOUT_FILENO, syn_err, strlen(syn_err)) < 0) {}
            continue;
        }

        if (pipeline.num_commands == 1 && strcmp(pipeline.commands[0].argv[0], "exit") == 0) {
            free_pipeline(&pipeline);
            break;
        }

        /* Execute pipeline directly in this tab process */
        shell_execute_pipeline(&tab->shell_ctx, &pipeline);

        /* If directory changed, notify GUI parent of new CWD */
        if (getcwd(tab->cwd, sizeof(tab->cwd))) {
            printf("[CWD]%s\n", tab->cwd);
            fflush(stdout);
        }

        /* Check background job notifications after command execution */
        notif[0] = '\0';
        jobs_check_status(&tab->shell_ctx.job_table, notif, sizeof(notif));
        if (notif[0] != '\0') {
            printf("%s", notif);
            fflush(stdout);
        }

        free_pipeline(&pipeline);

        if (tab->shell_ctx.should_exit) {
            break;
        }
    }
    _exit(0);
}

int gui_init(GuiApp *app, int width, int height, const char *title, int use_process_mode) {
    if (!app) return -1;
    XInitThreads();
    memset(app, 0, sizeof(GuiApp));
    app->use_process_mode = use_process_mode;

    app->display = XOpenDisplay(NULL);
    if (!app->display) app->display = XOpenDisplay(":0");
    if (!app->display) {
        fprintf(stderr, "terminator: cannot open X display. Is an X server or WSLg running?\n");
        return -1;
    }

    int x_fd = ConnectionNumber(app->display);
    if (x_fd >= 0) {
        int flags = fcntl(x_fd, F_GETFD);
        if (flags >= 0) fcntl(x_fd, F_SETFD, flags | FD_CLOEXEC);
    }

    app->screen = DefaultScreen(app->display);
    app->win_width = width;
    app->win_height = height;
    Colormap cm = DefaultColormap(app->display, app->screen);

    unsigned long col_black    = BlackPixel(app->display, app->screen);
    unsigned long col_red      = allocate_color(app->display, cm, 0xFFFF, 0x3333, 0x3333);
    unsigned long col_green    = allocate_color(app->display, cm, 0x3333, 0xFFFF, 0x3333);
    unsigned long col_output   = allocate_color(app->display, cm, 0xCCCC, 0xEEEE, 0xCCCC);
    unsigned long col_tab_bg   = allocate_color(app->display, cm, 0x1818, 0x1818, 0x1818);
    unsigned long col_tab_act  = allocate_color(app->display, cm, 0x2A2A, 0x4444, 0x2A2A);
    unsigned long col_tab_text = allocate_color(app->display, cm, 0x8888, 0xAAAA, 0x8888);
    unsigned long col_scroll   = allocate_color(app->display, cm, 0x5555, 0x7777, 0x5555);

    app->window = XCreateSimpleWindow(app->display, RootWindow(app->display, app->screen),
                                      100, 100, app->win_width, app->win_height, 1,
                                      col_green, col_black);

    XStoreName(app->display, app->window, title);
    XSetIconName(app->display, app->window, title);
    XSelectInput(app->display, app->window, KeyPressMask | ButtonPressMask | ExposureMask | StructureNotifyMask);

    const char *font_candidates[] = {
        "-misc-fixed-medium-r-normal--20-*-*-*-*-*-iso8859-1", "10x20",
        "-misc-fixed-medium-r-normal--18-*-*-*-*-*-iso8859-1", "9x18",
        "-misc-fixed-medium-r-normal--15-*-*-*-*-*-iso8859-1", "9x15",
        "fixed", NULL
    };
    app->font = NULL;
    for (int i = 0; font_candidates[i]; i++) {
        app->font = XLoadQueryFont(app->display, font_candidates[i]);
        if (app->font) break;
    }
    if (!app->font) app->font = XQueryFont(app->display, XGContextFromGC(DefaultGC(app->display, app->screen)));

    if (app->font) {
        app->font_width = app->font->max_bounds.width > 0 ? app->font->max_bounds.width : 8;
        app->font_ascent = app->font->ascent;
        app->font_height = (app->font->ascent + app->font->descent) > 0 ? (app->font->ascent + app->font->descent) : 16;
    } else {
        app->font_width = 8; app->font_height = 16; app->font_ascent = 12;
    }

    app->gc_bg             = create_gc(app->display, app->window, col_black, app->font);
    app->gc_prompt         = create_gc(app->display, app->window, col_red, app->font);
    app->gc_input          = create_gc(app->display, app->window, col_green, app->font);
    app->gc_output         = create_gc(app->display, app->window, col_output, app->font);
    app->gc_cursor         = create_gc(app->display, app->window, col_green, app->font);
    app->gc_tab_bar_bg     = create_gc(app->display, app->window, col_tab_bg, app->font);
    app->gc_tab_active_bg  = create_gc(app->display, app->window, col_tab_act, app->font);
    app->gc_tab_text       = create_gc(app->display, app->window, col_tab_text, app->font);
    app->gc_tab_active_text= create_gc(app->display, app->window, col_green, app->font);
    app->gc_scrollbar      = create_gc(app->display, app->window, col_scroll, NULL);

    app->back_buffer = XCreatePixmap(app->display, app->window, app->win_width, app->win_height,
                                     DefaultDepth(app->display, app->screen));

    app->wm_delete_window = XInternAtom(app->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app->display, app->window, &app->wm_delete_window, 1);

    app->atom_clipboard      = XInternAtom(app->display, "CLIPBOARD", False);
    app->atom_utf8_string    = XInternAtom(app->display, "UTF8_STRING", False);
    app->atom_targets        = XInternAtom(app->display, "TARGETS", False);
    app->atom_terminator_sel = XInternAtom(app->display, "TERMINATOR_SELECTION", False);

    hotkeys_init(&app->hotkeys);
    XMapWindow(app->display, app->window);
    XFlush(app->display);
    app->is_running = 1;

    gui_create_tab(app, NULL);
    return 0;
}

void gui_cleanup(GuiApp *app) {
    if (!app || !app->display) return;
    for (int i = 0; i < app->num_tabs; i++) {
        Tab *tab = app->tabs[i];
        if (tab) {
            if (tab->out_pipe_fd >= 0) close(tab->out_pipe_fd);
            if (tab->to_tab_fd >= 0) close(tab->to_tab_fd);

            if (tab->use_process_mode) {
                if (tab->tab_pid > 0) {
                    kill(-tab->tab_pid, SIGTERM);
                    kill(tab->tab_pid, SIGTERM);
                    if (waitpid(tab->tab_pid, NULL, WNOHANG) == 0) {
                        kill(-tab->tab_pid, SIGKILL);
                        kill(tab->tab_pid, SIGKILL);
                        waitpid(tab->tab_pid, NULL, 0);
                    }
                    tab->tab_pid = 0;
                }
            } else {
                if (tab->is_thread_alive) {
                    tab->is_thread_alive = 0;
                    pthread_cancel(tab->worker_thread);
                    pthread_join(tab->worker_thread, NULL);
                }
                if (tab->running_pid > 0) {
                    pid_t pg = tab->running_pgid > 0 ? tab->running_pgid : tab->running_pid;
                    kill(-pg, SIGTERM);
                    kill(tab->running_pid, SIGTERM);
                }
            }
            jobs_kill_all(&tab->shell_ctx.job_table);
            for (int k = 0; k < tab->num_completion_choices; k++) {
                if (tab->completion_choices[k]) free(tab->completion_choices[k]);
            }
            free(tab);
        }
    }
    app->num_tabs = 0;

    GC gcs[] = { app->gc_bg, app->gc_prompt, app->gc_input, app->gc_output, app->gc_cursor,
                 app->gc_tab_bar_bg, app->gc_tab_active_bg, app->gc_tab_text,
                 app->gc_tab_active_text, app->gc_scrollbar };
    for (size_t i = 0; i < sizeof(gcs)/sizeof(gcs[0]); i++) {
        if (gcs[i]) XFreeGC(app->display, gcs[i]);
    }
    if (app->font) XFreeFont(app->display, app->font);
    if (app->back_buffer) XFreePixmap(app->display, app->back_buffer);
    if (app->window) XDestroyWindow(app->display, app->window);
    XCloseDisplay(app->display);
    app->display = NULL;
}

/* ========================================================================= */
/*                         Tab Management                                    */
/* ========================================================================= */

int gui_create_tab(GuiApp *app, const char *initial_cwd) {
    if (!app || app->num_tabs >= MAX_TABS) return -1;

    Tab *tab = malloc(sizeof(Tab));
    if (!tab) return -1;
    memset(tab, 0, sizeof(Tab));

    tab->tab_id = app->num_tabs + 1;
    tab->use_process_mode = app->use_process_mode;
    tab->out_pipe_fd = -1;
    tab->to_tab_fd = -1;
    tab->history_pos = -1;
    shell_init(&tab->shell_ctx);

    if (initial_cwd) {
        snprintf(tab->cwd, sizeof(tab->cwd), "%s", initial_cwd);
    } else if (!getcwd(tab->cwd, sizeof(tab->cwd))) {
        strncpy(tab->cwd, "/", sizeof(tab->cwd) - 1);
    }
    snprintf(tab->shell_ctx.cwd, sizeof(tab->shell_ctx.cwd), "%s", tab->cwd);

    if (tab->use_process_mode) {
        /* Process Mode: each tab forks its own process and communicates with GUI via pipes */
        int gui_to_tab[2];
        int tab_to_gui[2];
        if (pipe(gui_to_tab) < 0 || pipe(tab_to_gui) < 0) {
            perror("terminator: pipe failed for Process Mode tab");
            free(tab);
            return -1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("terminator: fork failed for Process Mode tab");
            close(gui_to_tab[0]); close(gui_to_tab[1]);
            close(tab_to_gui[0]); close(tab_to_gui[1]);
            free(tab);
            return -1;
        }

        if (pid == 0) {
            setpgid(0, 0);
            /* The GUI process is the sole owner of the X11 connection.
             * DO NOT call XCloseDisplay() in the child process!
             * XCloseDisplay() flushes the buffer and sends protocol teardown packets
             * over the shared socket, which immediately desynchronizes XCB sequence
             * numbers in the parent GUI event loop ('Assertion !xcb_xlib_threads_sequence_lost failed').
             * Instead, simply close the child's inherited socket descriptor directly. */
            if (app->display) {
                int x_fd = ConnectionNumber(app->display);
                if (x_fd >= 0) close(x_fd);
                app->display = NULL;
            }
            close(gui_to_tab[1]);
            close(tab_to_gui[0]);
            tab_process_main(tab, gui_to_tab[0], tab_to_gui[1]);
            _exit(0);
        }

        close(gui_to_tab[0]);
        close(tab_to_gui[1]);
        tab->tab_pid = pid;
        tab->to_tab_fd = gui_to_tab[1];
        tab->out_pipe_fd = tab_to_gui[0];
        int flags = fcntl(tab->out_pipe_fd, F_GETFL, 0);
        fcntl(tab->out_pipe_fd, F_SETFL, flags | O_NONBLOCK);
    } else {
        /* Thread Mode: each tab is a dedicated POSIX thread */
        tab->is_thread_alive = 1;
        pthread_create(&tab->worker_thread, NULL, tab_worker_thread, tab);
    }

    int new_idx = app->num_tabs;
    app->tabs[new_idx] = tab;
    app->num_tabs++;
    app->active_tab_idx = new_idx;
    return new_idx;
}

void gui_close_tab(GuiApp *app, int tab_idx) {
    if (!app || tab_idx < 0 || tab_idx >= app->num_tabs) return;

    Tab *tab = app->tabs[tab_idx];
    if (tab->out_pipe_fd >= 0) close(tab->out_pipe_fd);
    if (tab->to_tab_fd >= 0) close(tab->to_tab_fd);

    if (tab->use_process_mode) {
        if (tab->tab_pid > 0) {
            kill(-tab->tab_pid, SIGTERM);
            kill(tab->tab_pid, SIGTERM);
            if (waitpid(tab->tab_pid, NULL, WNOHANG) == 0) {
                kill(-tab->tab_pid, SIGKILL);
                kill(tab->tab_pid, SIGKILL);
                waitpid(tab->tab_pid, NULL, 0);
            }
            tab->tab_pid = 0;
        }
    } else {
        if (tab->is_thread_alive) {
            tab->is_thread_alive = 0;
            pthread_cancel(tab->worker_thread);
            pthread_join(tab->worker_thread, NULL);
        }
        if (tab->running_pid > 0) {
            pid_t pg = tab->running_pgid > 0 ? tab->running_pgid : tab->running_pid;
            kill(-pg, SIGTERM);
            kill(tab->running_pid, SIGTERM);
        }
    }
    jobs_kill_all(&tab->shell_ctx.job_table);
    for (int k = 0; k < tab->num_completion_choices; k++) {
        if (tab->completion_choices[k]) free(tab->completion_choices[k]);
    }
    free(tab);

    for (int i = tab_idx; i < app->num_tabs - 1; i++) {
        app->tabs[i] = app->tabs[i + 1];
    }
    app->num_tabs--;
    app->tabs[app->num_tabs] = NULL;

    if (app->num_tabs == 0) {
        app->is_running = 0;
    } else if (tab_idx < app->active_tab_idx) {
        app->active_tab_idx--;
    } else if (app->active_tab_idx >= app->num_tabs) {
        app->active_tab_idx = app->num_tabs - 1;
    }
}

void gui_switch_tab(GuiApp *app, int tab_idx) {
    if (!app || tab_idx < 0 || tab_idx >= app->num_tabs) return;
    app->active_tab_idx = tab_idx;
    app->tabs[tab_idx]->scroll_offset = 0;
}

void tab_append_output(Tab *tab, const char *text, int len) {
    if (!tab || !text || len <= 0) return;

    /* Auto-scroll to bottom on new output */
    tab->scroll_offset = 0;

    /* Ensure output begins on the line below the command prompt, never on the same line */
    if (tab->num_lines > 0 && tab->lines[tab->num_lines - 1].type == LINE_TYPE_PROMPT_INPUT) {
        tab_new_line(tab, LINE_TYPE_OUTPUT, 0);
    }

    for (int i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') {
            tab_new_line(tab, LINE_TYPE_OUTPUT, 0);
            continue;
        }

        if (tab->num_lines == 0) tab_new_line(tab, LINE_TYPE_OUTPUT, 0);
        TermLine *cur = &tab->lines[tab->num_lines - 1];
        size_t cur_len = strlen(cur->text);

        if (cur_len >= MAX_LINE_CHARS - 2) {
            cur = tab_new_line(tab, LINE_TYPE_OUTPUT, 0);
            cur_len = 0;
        }
        cur->text[cur_len] = c;
        cur->text[cur_len + 1] = '\0';
    }
}

/* ========================================================================= */
/*                         Command Execution                                 */
/* ========================================================================= */

static void tab_execute_cmd_string(GuiApp *app, Tab *tab, const char *cmd_str);
static void tab_flush_pending_commands(GuiApp *app, Tab *tab);

static void tab_execute_cmd_string(GuiApp *app, Tab *tab, const char *cmd_str) {
    if (!app || !tab || !cmd_str) return;

    Pipeline pipeline;
    if (parse_line(cmd_str, &tab->shell_ctx, &pipeline) != 0) {
        tab_append_output(tab, "terminator: syntax error\n", 25);
        return;
    }

    if (pipeline.num_commands == 1 && is_builtin_command(pipeline.commands[0].argv[0])) {
        Command *cmd = &pipeline.commands[0];
        if (strcmp(cmd->argv[0], "exit") == 0) {
            free_pipeline(&pipeline);
            int cur_idx = app->active_tab_idx;
            gui_close_tab(app, cur_idx);
            gui_redraw(app);
            return;
        }
        if (strcmp(cmd->argv[0], "clear") == 0) {
            tab->num_lines = 0;
            tab->scroll_offset = 0;
            free_pipeline(&pipeline);
            gui_redraw(app);
            return;
        }
        if (strcmp(cmd->argv[0], "fg") == 0) {
            char out[512];
            Job res_job;
            int ret = jobs_builtin_fg(&tab->shell_ctx.job_table, cmd->argv, out, sizeof(out), &res_job);
            if (ret != 0) {
                if (out[0] != '\0') tab_append_output(tab, out, (int)strlen(out));
                tab->shell_ctx.last_exit_status = ret;
                free_pipeline(&pipeline);
                gui_redraw(app);
                return;
            }
            char fg_msg[600];
            snprintf(fg_msg, sizeof(fg_msg), "%s\n", res_job.command);
            tab_append_output(tab, fg_msg, (int)strlen(fg_msg));

            if (res_job.pgid > 0) {
                kill(-res_job.pgid, SIGCONT);
                kill(res_job.pid, SIGCONT);
            }
            tab->running_pid = res_job.pid;
            tab->running_pgid = res_job.pgid;
            snprintf(tab->running_cmd, sizeof(tab->running_cmd), "%s", res_job.command);
            tab->out_pipe_fd = res_job.out_pipe_fd;
            tab->is_running_cmd = 1;
            free_pipeline(&pipeline);
            gui_redraw(app);
            return;
        }

        if (strcasecmp(cmd->argv[0], "multiWatch") != 0) {
            char out[8192];
            out[0] = '\0';
            int ret = execute_builtin_captured(&tab->shell_ctx, cmd, out, sizeof(out));
            tab->shell_ctx.last_exit_status = ret;
            if (strcmp(cmd->argv[0], "cd") == 0 && ret == 0) {
                snprintf(tab->cwd, sizeof(tab->cwd), "%s", tab->shell_ctx.cwd);
            }
            if (out[0] != '\0') tab_append_output(tab, out, (int)strlen(out));
            free_pipeline(&pipeline);
            gui_redraw(app);
            return;
        }
    }

    int pipefds[2];
    if (pipe(pipefds) < 0) {
        perror("terminator: pipe failed");
        free_pipeline(&pipeline);
        return;
    }
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
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        if (chdir(tab->cwd) != 0) {}

        dup2(pipefds[1], STDOUT_FILENO);
        dup2(pipefds[1], STDERR_FILENO);
        close(pipefds[0]);
        close(pipefds[1]);

        if (pipeline.commands[0].input_file == NULL && pipeline.is_background) {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }

        /* Single command optimization: execute directly without wrapper fork */
        if (pipeline.num_commands == 1 && !is_builtin_command(pipeline.commands[0].argv[0])) {
            Command *c = &pipeline.commands[0];
            if (setup_command_redirections(c) != 0) {
                _exit(1);
            }
            execvp(c->argv[0], c->argv);
            if (errno == EACCES) {
                fprintf(stderr, "terminator: %s: Permission denied\n", c->argv[0]);
                _exit(126);
            }
            fprintf(stderr, "terminator: %s: command not found\n", c->argv[0]);
            _exit(127);
        }

        Pipeline exec_pl = pipeline;
        exec_pl.is_background = 0;
        int ret = shell_execute_pipeline(&tab->shell_ctx, &exec_pl);
        _exit(ret);
    }

    close(pipefds[1]);
    setpgid(pid, pid);

    if (pipeline.is_background) {
        Job *j = jobs_add(&tab->shell_ctx.job_table, pid, pid, cmd_str, JOB_STATE_RUNNING, pipefds[0]);
        char launch_msg[256];
        snprintf(launch_msg, sizeof(launch_msg), "[%d] %d\n", j ? j->id : 1, (int)pid);
        tab_append_output(tab, launch_msg, (int)strlen(launch_msg));
        gui_redraw(app);
    } else {
        tab->out_pipe_fd = pipefds[0];
        tab->running_pid = pid;
        tab->running_pgid = pid;
        strncpy(tab->running_cmd, cmd_str, sizeof(tab->running_cmd) - 1);
        tab->running_cmd[sizeof(tab->running_cmd) - 1] = '\0';
        tab->is_running_cmd = 1;
    }

    free_pipeline(&pipeline);
}

static void tab_flush_pending_commands(GuiApp *app, Tab *tab) {
    if (!app || !tab || tab->is_running_cmd || tab->pending_cmd_count <= 0) return;

    while (!tab->is_running_cmd && tab->pending_cmd_count > 0) {
        char next_cmd[sizeof(tab->pending_cmds[0])];
        snprintf(next_cmd, sizeof(next_cmd), "%s", tab->pending_cmds[0]);

        for (int i = 0; i < tab->pending_cmd_count - 1; i++) {
            strncpy(tab->pending_cmds[i], tab->pending_cmds[i + 1], sizeof(tab->pending_cmds[i]));
        }
        tab->pending_cmd_count--;

        tab_execute_cmd_string(app, tab, next_cmd);
    }
}

void tab_submit_command(GuiApp *app, Tab *tab) {
    if (!app || !tab) return;

    tab->scroll_offset = 0;
    const char *prompt_used = tab->is_multiline ? "> " : PROMPT_STRING;
    int p_len = (int)strlen(prompt_used);

    tab_record_prompt_input(app, tab, prompt_used, p_len);

    if (tab->input_len == 0 && !tab->is_multiline) return;

    if (!tab->is_multiline) {
        snprintf(tab->multiline_accum, sizeof(tab->multiline_accum), "%s", tab->input_buffer);
    } else {
        size_t acc_len = strlen(tab->multiline_accum);
        snprintf(tab->multiline_accum + acc_len, sizeof(tab->multiline_accum) - acc_len, "\n%s", tab->input_buffer);
    }

    tab->input_buffer[0] = '\0';
    tab->input_len = 0;
    tab->cursor_pos = 0;
    tab->history_pos = -1;
    tab->saved_input[0] = '\0';

    if (is_command_incomplete(tab->multiline_accum)) {
        tab->is_multiline = 1;
        return;
    }

    tab->is_multiline = 0;
    char cmd_str[sizeof(tab->multiline_accum)];
    snprintf(cmd_str, sizeof(cmd_str), "%s", tab->multiline_accum);
    tab->multiline_accum[0] = '\0';

    history_add(cmd_str);

    if (tab->use_process_mode) {
        if (tab->to_tab_fd >= 0) {
            if (write(tab->to_tab_fd, cmd_str, strlen(cmd_str)) < 0) {}
            if (write(tab->to_tab_fd, "\n", 1) < 0) {}
        }
        return;
    }

    if (tab->is_running_cmd) {
        if (tab->pending_cmd_count < MAX_PENDING_CMDS) {
            snprintf(tab->pending_cmds[tab->pending_cmd_count++], sizeof(tab->pending_cmds[0]), "%s", cmd_str);
        }
        return;
    }

    tab_execute_cmd_string(app, tab, cmd_str);
}

/* ========================================================================= */
/*                         Input Event Handling                              */
/* ========================================================================= */

void gui_handle_key_press(GuiApp *app, XKeyEvent *event) {
    if (!app || app->num_tabs == 0) return;
    Tab *tab = app->tabs[app->active_tab_idx];

    KeySym keysym;
    char buffer[32];
    int count = XLookupString(event, buffer, sizeof(buffer) - 1, &keysym, NULL);
    buffer[count] = '\0';

    int is_ctrl = (event->state & ControlMask);
    int is_shift = (event->state & ShiftMask);

    /* Task 9 Ctrl+R Interactive History Search Mode */
    if (tab->is_searching_history) {
        if (keysym == XK_Return || keysym == XK_KP_Enter) {
            char line[MAX_LINE_CHARS + 64];
            snprintf(line, sizeof(line), "Enter search term: %s\n", tab->search_query);
            tab_append_output(tab, line, (int)strlen(line));

            HistSearchResult res;
            history_search(tab->search_query, &res);
            if (res.match_type == HIST_MATCH_NONE || res.count == 0) {
                tab_append_output(tab, "No match for search term in history\n", 36);
            } else {
                for (size_t i = 0; i < res.count; i++) {
                    char entry_line[MAX_LINE_CHARS + 2];
                    snprintf(entry_line, sizeof(entry_line), "%s\n", res.entries[i]);
                    tab_append_output(tab, entry_line, (int)strlen(entry_line));
                }
                /* Load the best / most recent match into prompt input */
                snprintf(tab->input_buffer, sizeof(tab->input_buffer), "%s", res.entries[0]);
                tab->input_len = (int)strlen(tab->input_buffer);
                tab->cursor_pos = tab->input_len;
            }
            hist_search_result_free(&res);
            tab->is_searching_history = 0;
            tab->search_query[0] = '\0';
            tab->search_query_len = 0;
            gui_redraw(app);
            return;
        }
        if (keysym == XK_Escape || (is_ctrl && (keysym == XK_c || keysym == XK_C))) {
            tab->is_searching_history = 0;
            tab->search_query[0] = '\0';
            tab->search_query_len = 0;
            gui_redraw(app);
            return;
        }
        if (keysym == XK_BackSpace) {
            if (tab->search_query_len > 0) {
                tab->search_query[--tab->search_query_len] = '\0';
                gui_redraw(app);
            }
            return;
        }
        if (count > 0 && buffer[0] >= 32 && buffer[0] <= 126) {
            if (tab->search_query_len < (int)sizeof(tab->search_query) - 1) {
                tab->search_query[tab->search_query_len++] = buffer[0];
                tab->search_query[tab->search_query_len] = '\0';
                gui_redraw(app);
            }
            return;
        }
        return;
    }

    if (is_ctrl && !is_shift && (keysym == XK_R || keysym == XK_r)) {
        tab->is_searching_history = 1;
        tab->search_query[0] = '\0';
        tab->search_query_len = 0;
        gui_redraw(app);
        return;
    }

    /* Task 10 Numbered Candidate Selection Mode */
    if (tab->is_completion_active) {
        if (!is_ctrl && count > 0 && buffer[0] >= '1' && buffer[0] <= '9') {
            int sel = buffer[0] - '1';
            if (sel < tab->num_completion_choices && tab->completion_choices[sel]) {
                const char *chosen = tab->completion_choices[sel];
                char esc[COMPLETE_MAX_WORD * 2];
                complete_escape_filename(chosen, esc, sizeof(esc));
                char repl[PATH_MAX + COMPLETE_MAX_WORD * 2];
                snprintf(repl, sizeof(repl), "%s%s", tab->completion_dir_prefix, esc);
                size_t rlen = strlen(repl);
                if (repl[rlen - 1] != '/') {
                    if (rlen + 1 < sizeof(repl)) {
                        repl[rlen++] = ' ';
                        repl[rlen] = '\0';
                    }
                }
                tab_delete_range(tab, (int)tab->completion_word_start,
                                 (int)(tab->completion_word_start + tab->completion_word_len));
                for (size_t i = 0; i < rlen; i++) {
                    tab_insert_char(tab, repl[i]);
                }
            }
            for (int i = 0; i < tab->num_completion_choices; i++) {
                if (tab->completion_choices[i]) free(tab->completion_choices[i]);
                tab->completion_choices[i] = NULL;
            }
            tab->num_completion_choices = 0;
            tab->is_completion_active = 0;
            gui_redraw(app);
            return;
        }
        /* Any other key dismisses candidate selection mode and falls through */
        for (int i = 0; i < tab->num_completion_choices; i++) {
            if (tab->completion_choices[i]) free(tab->completion_choices[i]);
            tab->completion_choices[i] = NULL;
        }
        tab->num_completion_choices = 0;
        tab->is_completion_active = 0;
    }

    /* Task 10 Tab File-Name Auto-Completion */
    if (keysym == XK_Tab) {
        CompletionResult cr;
        if (complete_file_candidates(tab->input_buffer, (size_t)tab->cursor_pos, tab->cwd, &cr) == 0) {
            if (cr.count == 1) {
                char esc[COMPLETE_MAX_WORD * 2];
                complete_escape_filename(cr.items[0], esc, sizeof(esc));
                char repl[PATH_MAX + COMPLETE_MAX_WORD * 2];
                snprintf(repl, sizeof(repl), "%s%s", cr.dir_prefix, esc);
                size_t rlen = strlen(repl);
                if (repl[rlen - 1] != '/') {
                    if (rlen + 1 < sizeof(repl)) {
                        repl[rlen++] = ' ';
                        repl[rlen] = '\0';
                    }
                }
                tab_delete_range(tab, (int)cr.word_start, (int)(cr.word_start + cr.word_len));
                for (size_t i = 0; i < rlen; i++) {
                    tab_insert_char(tab, repl[i]);
                }
            } else if (cr.count > 1) {
                char esc_lcp[COMPLETE_MAX_WORD * 2];
                complete_escape_filename(cr.lcp, esc_lcp, sizeof(esc_lcp));
                char lcp_repl[PATH_MAX + COMPLETE_MAX_WORD * 2];
                snprintf(lcp_repl, sizeof(lcp_repl), "%s%s", cr.dir_prefix, esc_lcp);
                size_t lcp_len = strlen(lcp_repl);
                size_t cur_word_len = cr.word_len;

                if (lcp_len > cur_word_len) {
                    tab_delete_range(tab, (int)cr.word_start, (int)(cr.word_start + cr.word_len));
                    for (size_t i = 0; i < lcp_len; i++) {
                        tab_insert_char(tab, lcp_repl[i]);
                    }
                    cur_word_len = lcp_len;
                }

                char *list_str = complete_format_numbered_list(&cr, 80);
                if (list_str) {
                    const char *p_used = tab->is_multiline ? "> " : PROMPT_STRING;
                    tab_record_prompt_input(app, tab, p_used, (int)strlen(p_used));
                    tab_append_output(tab, list_str, (int)strlen(list_str));
                    tab_append_output(tab, "\n", 1);
                    free(list_str);
                }

                tab->num_completion_choices = cr.count > 9 ? 9 : (int)cr.count;
                for (int i = 0; i < tab->num_completion_choices; i++) {
                    tab->completion_choices[i] = strdup(cr.items[i]);
                }
                tab->completion_word_start = cr.word_start;
                tab->completion_word_len = cur_word_len;
                snprintf(tab->completion_dir_prefix, sizeof(tab->completion_dir_prefix), "%s", cr.dir_prefix);
                tab->is_completion_active = 1;
            }
            completion_result_free(&cr);
            gui_redraw(app);
        }
        return;
    }

    /* Tab Creation / Closure shortcuts */
    if (is_ctrl && (keysym == XK_T || keysym == XK_t)) {
        gui_create_tab(app, tab->cwd);
        gui_redraw(app);
        return;
    }
    if (is_ctrl && is_shift && (keysym == XK_W || keysym == XK_w)) {
        gui_close_tab(app, app->active_tab_idx);
        gui_redraw(app);
        return;
    }

    /* Signals: Ctrl+C and Ctrl+Z */
    if (is_ctrl && !is_shift && (keysym == XK_C || keysym == XK_c)) {
        if (tab->use_process_mode && tab->tab_pid > 0) {
            kill(tab->tab_pid, SIGINT);
        } else if (tab->is_running_cmd && tab->running_pid > 0) {
            pid_t target = tab->running_pgid > 0 ? tab->running_pgid : tab->running_pid;
            kill(-target, SIGINT);
            kill(tab->running_pid, SIGINT);
            tab->pending_cmd_count = 0;
        } else {
            tab->input_buffer[0] = '\0';
            tab->input_len = 0;
            tab->cursor_pos = 0;
            tab->history_pos = -1;
            tab->pending_cmd_count = 0;
        }
        tab_append_output(tab, "^C\n", 3);
        gui_redraw(app);
        return;
    }

    if (is_ctrl && !is_shift && (keysym == XK_Z || keysym == XK_z)) {
        if (tab->use_process_mode && tab->tab_pid > 0) {
            kill(tab->tab_pid, SIGTSTP);
            tab_append_output(tab, "^Z\n", 3);
            gui_redraw(app);
            return;
        } else if (tab->is_running_cmd && tab->running_pid > 0) {
            pid_t target = tab->running_pgid > 0 ? tab->running_pgid : tab->running_pid;
            kill(-target, SIGTSTP);
            kill(tab->running_pid, SIGTSTP);

            Job *j = jobs_add(&tab->shell_ctx.job_table, target, tab->running_pid,
                              tab->running_cmd, JOB_STATE_STOPPED, tab->out_pipe_fd);
            char stop_msg[MAX_LINE_CHARS + 128];
            snprintf(stop_msg, sizeof(stop_msg), "^Z\n[%d]+  Stopped                 %s\n",
                     j ? j->id : 1, tab->running_cmd);
            tab_append_output(tab, stop_msg, (int)strlen(stop_msg));

            tab->is_running_cmd = 0;
            tab->running_pid = 0;
            tab->running_pgid = 0;
            tab->out_pipe_fd = -1;
            tab->running_cmd[0] = '\0';

            tab_flush_pending_commands(app, tab);
            gui_redraw(app);
            return;
        }
    }

    /* Tab navigation via keyboard */
    if (is_shift && keysym == XK_Left && app->active_tab_idx > 0) {
        gui_switch_tab(app, app->active_tab_idx - 1);
        gui_redraw(app);
        return;
    }
    if (is_shift && keysym == XK_Right && app->active_tab_idx < app->num_tabs - 1) {
        gui_switch_tab(app, app->active_tab_idx + 1);
        gui_redraw(app);
        return;
    }

    /* Basic cursor movement & submission */
    if (keysym == XK_Home) { tab->cursor_pos = 0; gui_redraw(app); return; }
    if (keysym == XK_End)  { tab->cursor_pos = tab->input_len; gui_redraw(app); return; }
    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        tab_submit_command(app, tab);
        gui_redraw(app);
        return;
    }
    if (keysym == XK_BackSpace) {
        if (tab->cursor_pos > 0) {
            tab_delete_range(tab, tab->cursor_pos - 1, tab->cursor_pos);
            gui_redraw(app);
        }
        return;
    }
    if (keysym == XK_Delete) {
        if (tab->cursor_pos < tab->input_len) {
            tab_delete_range(tab, tab->cursor_pos, tab->cursor_pos + 1);
            gui_redraw(app);
        }
        return;
    }
    if (keysym == XK_Left && !is_ctrl && !is_shift) {
        if (tab->cursor_pos > 0) { tab->cursor_pos--; gui_redraw(app); }
        return;
    }
    if (keysym == XK_Right && !is_ctrl && !is_shift) {
        if (tab->cursor_pos < tab->input_len) { tab->cursor_pos++; gui_redraw(app); }
        return;
    }

    /* Task 7 Configurable Hotkeys */
    int cols = get_term_cols(app);
    const char *active_p = tab->is_multiline ? "> " : PROMPT_STRING;
    int p_len = (int)strlen(active_p);
    int first_cap = get_first_line_capacity(app, p_len);

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
                tab_delete_range(tab, 0, tab->cursor_pos);
                gui_redraw(app);
                return;
            case HOTKEY_DELETE_TO_END:
                tab_delete_range(tab, tab->cursor_pos, tab->input_len);
                gui_redraw(app);
                return;
            case HOTKEY_DELETE_WORD: {
                int end = tab->cursor_pos, start = end;
                while (start > 0 && isspace((unsigned char)tab->input_buffer[start - 1])) start--;
                while (start > 0 && !isspace((unsigned char)tab->input_buffer[start - 1])) start--;
                tab_delete_range(tab, start, end);
                gui_redraw(app);
                return;
            }
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
                if (tab->input_len > first_cap && tab->cursor_pos > first_cap) {
                    int rel = tab->cursor_pos - first_cap;
                    int row = 1 + (rel / cols);
                    if (row == 1) {
                        int col = rel % cols;
                        int target = col - p_len;
                        if (target < 0) target = 0;
                        if (target > first_cap) target = first_cap;
                        tab->cursor_pos = target;
                    } else {
                        tab->cursor_pos -= cols;
                    }
                    gui_redraw(app);
                    return;
                }
                int h_count = history_count();
                if (h_count > 0) {
                    if (tab->history_pos == -1) {
                        snprintf(tab->saved_input, sizeof(tab->saved_input), "%s", tab->input_buffer);
                        tab->history_pos = 0;
                    } else if (tab->history_pos + 1 < h_count) {
                        tab->history_pos++;
                    }
                    char h_cmd[MAX_LINE_CHARS];
                    if (history_get_relative(tab->history_pos, h_cmd, sizeof(h_cmd)) == 0) {
                        snprintf(tab->input_buffer, sizeof(tab->input_buffer), "%s", h_cmd);
                        tab->input_len = (int)strlen(tab->input_buffer);
                        tab->cursor_pos = tab->input_len;
                        gui_redraw(app);
                        return;
                    }
                }
                break;
            case HOTKEY_HISTORY_NEXT:
                if (tab->input_len > first_cap) {
                    int total_rows = 1 + (tab->input_len - first_cap + cols - 1) / cols;
                    int cur_row = (tab->cursor_pos <= first_cap) ? 0 : 1 + ((tab->cursor_pos - first_cap) / cols);
                    if (cur_row < total_rows - 1) {
                        int target = (cur_row == 0) ? (first_cap + tab->cursor_pos) : (tab->cursor_pos + cols);
                        if (target > tab->input_len) target = tab->input_len;
                        tab->cursor_pos = target;
                        gui_redraw(app);
                        return;
                    }
                }
                if (tab->history_pos >= 0) {
                    if (tab->history_pos > 0) {
                        tab->history_pos--;
                        char h_cmd[MAX_LINE_CHARS];
                        if (history_get_relative(tab->history_pos, h_cmd, sizeof(h_cmd)) == 0) {
                            snprintf(tab->input_buffer, sizeof(tab->input_buffer), "%s", h_cmd);
                        }
                    } else {
                        tab->history_pos = -1;
                        snprintf(tab->input_buffer, sizeof(tab->input_buffer), "%s", tab->saved_input);
                    }
                    tab->input_len = (int)strlen(tab->input_buffer);
                    tab->cursor_pos = tab->input_len;
                    gui_redraw(app);
                    return;
                }
                break;
            default:
                break;
        }
    }

    /* Scroll navigation via PageUp/PageDown or Shift+Up/Shift+Down */
    int max_scroll = get_max_scroll(app, tab);
    if (keysym == XK_Page_Up || (is_shift && keysym == XK_Up)) {
        tab->scroll_offset += (keysym == XK_Page_Up ? 10 : 1);
        if (tab->scroll_offset > max_scroll) tab->scroll_offset = max_scroll;
        gui_redraw(app);
        return;
    }
    if (keysym == XK_Page_Down || (is_shift && keysym == XK_Down)) {
        tab->scroll_offset -= (keysym == XK_Page_Down ? 10 : 1);
        if (tab->scroll_offset < 0) tab->scroll_offset = 0;
        gui_redraw(app);
        return;
    }

    /* Printable ASCII character entry */
    if (count > 0 && buffer[0] >= 32 && buffer[0] <= 126) {
        tab_insert_char(tab, buffer[0]);
        gui_redraw(app);
    }
}

void gui_handle_button_press(GuiApp *app, XButtonEvent *event) {
    if (!app || app->num_tabs == 0) return;
    Tab *tab = app->tabs[app->active_tab_idx];

    int max_scroll = get_max_scroll(app, tab);
    if (event->button == 4) {
        tab->scroll_offset += 3;
        if (tab->scroll_offset > max_scroll) tab->scroll_offset = max_scroll;
        gui_redraw(app);
        return;
    }
    if (event->button == 5) {
        tab->scroll_offset -= 3;
        if (tab->scroll_offset < 0) tab->scroll_offset = 0;
        gui_redraw(app);
        return;
    }

    if (event->y > TAB_BAR_HEIGHT) return;

    int plus_x, plus_w, tab_w, spacing;
    get_tab_bar_layout(app, &plus_x, &plus_w, &tab_w, &spacing);

    if (event->x >= plus_x && event->x <= plus_x + plus_w) {
        gui_create_tab(app, tab->cwd);
        gui_redraw(app);
        return;
    }

    int cur_x = 10;
    for (int i = 0; i < app->num_tabs; i++) {
        if (event->x >= cur_x && event->x < cur_x + tab_w) {
            gui_switch_tab(app, i);
            gui_redraw(app);
            return;
        }
        cur_x += tab_w + spacing;
    }
}

/* ========================================================================= */
/*                         Window Redraw (Double-Buffered)                   */
/* ========================================================================= */

void gui_redraw(GuiApp *app) {
    if (!app || !app->display || app->num_tabs == 0) return;
    Tab *tab = app->tabs[app->active_tab_idx];

    XFillRectangle(app->display, app->back_buffer, app->gc_bg, 0, 0, app->win_width, app->win_height);
    XFillRectangle(app->display, app->back_buffer, app->gc_tab_bar_bg, 0, 0, app->win_width, TAB_BAR_HEIGHT);

    int plus_x, plus_w, tab_w, spacing;
    get_tab_bar_layout(app, &plus_x, &plus_w, &tab_w, &spacing);
    int tab_text_y = TAB_BAR_HEIGHT - 8;

    /* Render docked '+' button */
    const char *plus_label = " [ + ] ";
    XFillRectangle(app->display, app->back_buffer, app->gc_tab_active_bg, plus_x, 3, plus_w, TAB_BAR_HEIGHT - 6);
    XDrawString(app->display, app->back_buffer, app->gc_tab_active_text, plus_x + 4, tab_text_y, plus_label, (int)strlen(plus_label));

    /* Render Tab buttons */
    int cur_x = 10;
    for (int i = 0; i < app->num_tabs; i++) {
        char label[128];
        const char *cwd = app->tabs[i]->cwd;
        const char *last_slash = strrchr(cwd, '/');
        const char *short_name = last_slash ? (last_slash + 1) : cwd;
        if (!short_name || short_name[0] == '\0') short_name = "/";

        int max_chars = (tab_w - 10) / app->font_width;
        if (max_chars <= 4) {
            snprintf(label, sizeof(label), " %d ", i + 1);
        } else {
            int avail = max_chars - 7;
            if (avail > 0 && (int)strlen(short_name) > avail) {
                snprintf(label, sizeof(label), " [ %d: %.*s.. ] ", i + 1, avail > 2 ? avail - 2 : 1, short_name);
            } else {
                snprintf(label, sizeof(label), " [ %d: %.30s ] ", i + 1, short_name);
            }
        }
        int label_len = (int)strlen(label);

        if (i == app->active_tab_idx) {
            XFillRectangle(app->display, app->back_buffer, app->gc_tab_active_bg, cur_x, 3, tab_w, TAB_BAR_HEIGHT - 6);
            XDrawString(app->display, app->back_buffer, app->gc_tab_active_text, cur_x + 5, tab_text_y, label, label_len);
        } else {
            XDrawString(app->display, app->back_buffer, app->gc_tab_text, cur_x + 5, tab_text_y, label, label_len);
        }
        cur_x += tab_w + spacing;
    }

    /* Content Area & Dimensions */
    int cols = get_term_cols(app);
    const char *active_p = tab->is_searching_history ? "Enter search term: " :
                           (tab->is_multiline ? "> " : PROMPT_STRING);
    int p_len = (int)strlen(active_p);
    int first_cap = get_first_line_capacity(app, p_len);

    const char *cur_input_buf = tab->is_searching_history ? tab->search_query : tab->input_buffer;
    int cur_input_len = tab->is_searching_history ? tab->search_query_len : tab->input_len;
    int cur_cursor_pos = tab->is_searching_history ? tab->search_query_len : tab->cursor_pos;

    int active_rows = 1;
    if (cur_input_len > first_cap) {
        active_rows = 1 + (cur_input_len - first_cap + cols - 1) / cols;
    }

    int content_top = TAB_BAR_HEIGHT + 10;
    int visible_rows = (app->win_height - content_top - (active_rows * app->font_height)) / app->font_height;
    if (visible_rows <= 0) visible_rows = 1;

    int max_scroll = tab->num_lines > visible_rows ? (tab->num_lines - visible_rows) : 0;
    if (tab->scroll_offset > max_scroll) tab->scroll_offset = max_scroll;
    if (tab->scroll_offset < 0) tab->scroll_offset = 0;

    int start_l = max_scroll - tab->scroll_offset;
    int end_l = start_l + visible_rows;
    if (end_l > tab->num_lines) end_l = tab->num_lines;

    int cur_y = content_top + app->font_ascent;
    for (int i = start_l; i < end_l; i++) {
        TermLine *line = &tab->lines[i];
        if (line->type == LINE_TYPE_PROMPT_INPUT) {
            if (line->prompt_len > 0) {
                XDrawString(app->display, app->back_buffer, app->gc_prompt, 10, cur_y, line->text, line->prompt_len);
            }
            int cmd_len = (int)strlen(line->text) - line->prompt_len;
            if (cmd_len > 0) {
                XDrawString(app->display, app->back_buffer, app->gc_input,
                            10 + (line->prompt_len * app->font_width), cur_y,
                            line->text + line->prompt_len, cmd_len);
            }
        } else {
            XDrawString(app->display, app->back_buffer, app->gc_output, 10, cur_y, line->text, (int)strlen(line->text));
        }
        cur_y += app->font_height;
    }

    /* Active Input & Wrapped Lines */
    XDrawString(app->display, app->back_buffer, app->gc_prompt, 10, cur_y, active_p, p_len);
    int line0_chars = cur_input_len < first_cap ? cur_input_len : first_cap;
    if (line0_chars > 0) {
        XDrawString(app->display, app->back_buffer, app->gc_input,
                    10 + (p_len * app->font_width), cur_y, cur_input_buf, line0_chars);
    }

    if (cur_input_len > first_cap) {
        int off = first_cap;
        int row = 1;
        while (off < cur_input_len) {
            int chunk = cur_input_len - off > cols ? cols : cur_input_len - off;
            XDrawString(app->display, app->back_buffer, app->gc_input,
                        10, cur_y + (row * app->font_height), cur_input_buf + off, chunk);
            off += chunk;
            row++;
        }
    }

    /* Unified Block Cursor Calculation */
    int c_row = 0, c_col = 0;
    if (cur_cursor_pos <= first_cap) {
        c_row = 0;
        c_col = p_len + cur_cursor_pos;
    } else {
        int rel = cur_cursor_pos - first_cap;
        c_row = 1 + (rel / cols);
        c_col = rel % cols;
    }
    XFillRectangle(app->display, app->back_buffer, app->gc_cursor,
                   10 + (c_col * app->font_width),
                   cur_y + (c_row * app->font_height) - app->font_ascent,
                   app->font_width, app->font_height);

    /* Scrollbar */
    if (tab->num_lines > visible_rows) {
        int total_bar = app->win_height - content_top - 5;
        int thumb_h = (visible_rows * total_bar) / tab->num_lines;
        if (thumb_h < 15) thumb_h = 15;
        int thumb_y = content_top + (int)(((long)start_l * (total_bar - thumb_h)) / (tab->num_lines - visible_rows));
        XFillRectangle(app->display, app->back_buffer, app->gc_scrollbar, app->win_width - 8, thumb_y, 5, thumb_h);
    }

    XCopyArea(app->display, app->back_buffer, app->window, app->gc_bg, 0, 0, app->win_width, app->win_height, 0, 0);
    XFlush(app->display);
}

/* ========================================================================= */
/*                         Main Event Loop                                   */
/* ========================================================================= */

void gui_run_loop(GuiApp *app) {
    if (!app || !app->display) return;
    int x_fd = ConnectionNumber(app->display);

    while (app->is_running) {
        while (XPending(app->display) > 0) {
            XEvent ev;
            XNextEvent(app->display, &ev);

            switch (ev.type) {
                case Expose:
                    gui_redraw(app);
                    break;

                case ConfigureNotify:
                    if (ev.xconfigure.width != app->win_width || ev.xconfigure.height != app->win_height) {
                        app->win_width = ev.xconfigure.width;
                        app->win_height = ev.xconfigure.height;
                        if (app->back_buffer) XFreePixmap(app->display, app->back_buffer);
                        app->back_buffer = XCreatePixmap(app->display, app->window, app->win_width, app->win_height,
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
                    if ((Atom)ev.xclient.data.l[0] == app->wm_delete_window) {
                        app->is_running = 0;
                    }
                    break;

                case SelectionRequest: {
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
                        XChangeProperty(req->display, req->requestor, req->property, XA_ATOM, 32, PropModeReplace,
                                        (unsigned char *)targets, sizeof(targets) / sizeof(Atom));
                        reply.xselection.property = req->property;
                    } else if (req->target == XA_STRING || req->target == app->atom_utf8_string) {
                        XChangeProperty(req->display, req->requestor, req->property, req->target, 8, PropModeReplace,
                                        (unsigned char *)app->clipboard_data, app->clipboard_len);
                        reply.xselection.property = req->property;
                    }
                    XSendEvent(req->display, req->requestor, False, 0, &reply);
                    XFlush(app->display);
                    break;
                }

                case SelectionNotify: {
                    XSelectionEvent *sel = &ev.xselection;
                    if (sel->property != None) {
                        Atom type_ret;
                        int format_ret;
                        unsigned long nitems, bytes_after;
                        unsigned char *prop = NULL;
                        if (XGetWindowProperty(app->display, app->window, sel->property, 0, 4096, True,
                                               AnyPropertyType, &type_ret, &format_ret, &nitems, &bytes_after, &prop) == Success && prop) {
                            if (app->num_tabs > 0) {
                                Tab *t = app->tabs[app->active_tab_idx];
                                for (unsigned long i = 0; i < nitems; i++) {
                                    char c = prop[i];
                                    if (c != '\r' && c != '\n' && c >= 32 && c <= 126) tab_insert_char(t, c);
                                }
                                gui_redraw(app);
                            }
                            XFree(prop);
                        }
                    }
                    break;
                }

                default:
                    break;
            }
        }

        if (!app->is_running) break;

        #define MAX_POLL_ENTRIES (1 + MAX_TABS * (1 + MAX_JOBS))
        struct pollfd fds[MAX_POLL_ENTRIES];
        int poll_tab_idx[MAX_POLL_ENTRIES];
        int poll_is_bg[MAX_POLL_ENTRIES];
        int poll_job_id[MAX_POLL_ENTRIES];
        int num_poll_fds = 1;

        fds[0].fd = x_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        for (int i = 0; i < app->num_tabs; i++) {
            Tab *t = app->tabs[i];
            if (!t) continue;
            if ((t->is_running_cmd || t->use_process_mode) && t->out_pipe_fd >= 0 && num_poll_fds < MAX_POLL_ENTRIES) {
                fds[num_poll_fds].fd = t->out_pipe_fd;
                fds[num_poll_fds].events = POLLIN | POLLHUP;
                fds[num_poll_fds].revents = 0;
                poll_tab_idx[num_poll_fds] = i;
                poll_is_bg[num_poll_fds] = 0;
                poll_job_id[num_poll_fds] = -1;
                num_poll_fds++;
            }
            for (int j = 0; j < t->shell_ctx.job_table.num_jobs; j++) {
                Job *job = &t->shell_ctx.job_table.jobs[j];
                if (job->state == JOB_STATE_RUNNING && job->out_pipe_fd >= 0 && num_poll_fds < MAX_POLL_ENTRIES) {
                    fds[num_poll_fds].fd = job->out_pipe_fd;
                    fds[num_poll_fds].events = POLLIN | POLLHUP;
                    fds[num_poll_fds].revents = 0;
                    poll_tab_idx[num_poll_fds] = i;
                    poll_is_bg[num_poll_fds] = 1;
                    poll_job_id[num_poll_fds] = job->id;
                    num_poll_fds++;
                }
            }
        }

        int ret = poll(fds, num_poll_fds, 50);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        int needs_redraw = 0;
        for (int i = 1; i < num_poll_fds; i++) {
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                int t_idx = poll_tab_idx[i];
                Tab *t = app->tabs[t_idx];
                char buf[512];
                ssize_t n = read(fds[i].fd, buf, sizeof(buf));

                if (n > 0) {
                    if (t->use_process_mode && strncmp(buf, "[CWD]", 5) == 0) {
                        char *nl = strchr(buf, '\n');
                        if (nl) {
                            *nl = '\0';
                            strncpy(t->cwd, buf + 5, sizeof(t->cwd) - 1);
                            t->cwd[sizeof(t->cwd) - 1] = '\0';
                            if (nl + 1 < buf + n) {
                                tab_append_output(t, nl + 1, (int)(n - (nl + 1 - buf)));
                            }
                        }
                    } else {
                        tab_append_output(t, buf, (int)n);
                    }
                    if (t_idx == app->active_tab_idx) needs_redraw = 1;
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    if (poll_is_bg[i]) {
                        close(fds[i].fd);
                        Job *bj = jobs_find_by_id(&t->shell_ctx.job_table, poll_job_id[i]);
                        if (bj) bj->out_pipe_fd = -1;
                    } else {
                        if (t->use_process_mode) {
                            gui_close_tab(app, t_idx);
                            needs_redraw = 1;
                            break;
                        } else {
                            close(t->out_pipe_fd);
                            t->out_pipe_fd = -1;

                            int status = 0;
                            waitpid(t->running_pid, &status, WNOHANG);
                            if (WIFEXITED(status)) t->shell_ctx.last_exit_status = WEXITSTATUS(status);
                            else if (WIFSIGNALED(status)) t->shell_ctx.last_exit_status = 128 + WTERMSIG(status);

                            t->is_running_cmd = 0;
                            t->running_pid = 0;
                            t->running_pgid = 0;
                            t->running_cmd[0] = '\0';

                            tab_flush_pending_commands(app, t);
                        }
                        if (t_idx == app->active_tab_idx) needs_redraw = 1;
                    }
                }
            }
        }

        if (app->num_tabs == 0 || !app->is_running) {
            app->is_running = 0;
            break;
        }

        for (int i = 0; i < app->num_tabs; i++) {
            Tab *t = app->tabs[i];
            if (!t) continue;
            if (t->use_process_mode) continue;
            char notif[1024];
            notif[0] = '\0';
            jobs_check_status(&t->shell_ctx.job_table, notif, sizeof(notif));
            if (notif[0] != '\0') {
                tab_append_output(t, notif, (int)strlen(notif));
                if (i == app->active_tab_idx) needs_redraw = 1;
            }
        }

        if (needs_redraw) gui_redraw(app);
    }
}
