#ifndef MULTIWATCH_H
#define MULTIWATCH_H

#include "shell.h"

#define MAX_MW_COMMANDS 32
#ifndef MAX_LINE_CHARS
#define MAX_LINE_CHARS 1024
#endif

/**
 * @brief Built-in command: Runs multiple commands in parallel, re-running each every 2 seconds.
 *
 * Syntax:
 *     multiWatch ["cmd1", "cmd2", ...]
 *
 * Each command executes in its own child process with an independent pipe.
 * Output is collected using poll() and printed with timestamps until interrupted by Ctrl+C.
 *
 * @param[in,out] ctx Pointer to active ShellContext.
 * @param[in] argc Number of arguments passed to multiWatch.
 * @param[in] argv Argument array (argv[0]="multiWatch").
 * @return 0 on success / interruption, 1 on syntax or execution failure.
 */
int builtin_multiwatch(ShellContext *ctx, int argc, char **argv);

#endif /* MULTIWATCH_H */
