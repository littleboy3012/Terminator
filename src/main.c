#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "shell.h"
#include "parser.h"
#include "gui.h"
#include "hotkeys.h"
#include "history.h"

/**
 * @brief Entry point for Terminator.
 *
 * Supports exactly two execution models per specification:
 * - Thread Mode (default): ./terminator
 *   Each tab runs as a dedicated POSIX thread (pthread_t) sharing application memory.
 * - Process Mode: ./terminator useProcess=True
 *   Each tab runs as a dedicated child process created via fork(), communicating
 *   with the GUI parent through UNIX pipes and POSIX shared memory.
 *
 * In both modes, Terminator renders its visual interface in an X11 window using Xlib.
 *
 * @param[in] argc Number of command-line arguments.
 * @param[in] argv Array of argument strings.
 * @return 0 on normal exit, or non-zero exit status code.
 */
int main(int argc, char *argv[]) {
    int use_process_mode = 0;

    /* Parse execution mode */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "useProcess=True") == 0) {
            use_process_mode = 1;
        }
    }

    if (use_process_mode) {
        printf("[Terminator] Mode: Process Mode (useProcess=True)\n");
    } else {
        printf("[Terminator] Mode: Thread Mode (default)\n");
    }
    fflush(stdout);

    /* Initialize shared history (POSIX shared memory in process mode, heap in thread mode) */
    if (history_init(use_process_mode) != 0) {
        fprintf(stderr, "terminator: failed to initialize shared history\n");
        return 1;
    }

    /* Ensure ~/.terminatorrc exists and custom hotkeys are loaded */
    HotkeyConfig dummy_hk;
    hotkeys_init(&dummy_hk);

    /* Launch the X11 Graphical User Interface */
    GuiApp app;
    if (gui_init(&app, 900, 600, "Terminator", use_process_mode) != 0) {
        fprintf(stderr, "terminator: failed to open X11 display. Is an X server or WSLg running?\n");
        history_cleanup();
        return 1;
    }

    /* Run the non-blocking X11 event loop */
    gui_run_loop(&app);

    /* Free all X11 and thread/process resources upon window closure */
    gui_cleanup(&app);
    history_cleanup();

    return 0;
}
