# Terminator: Comprehensive System Design & Execution Architecture
**Computing Lab (CS69201) · Term Project · IIT Kharagpur**

---

## 1. System Architecture & Module Inventory

### 1.1 High-Level Architecture Overview

Terminator is a dual-paradigm Unix terminal emulator and POSIX-compliant command shell developed in C11 for Linux. It combines a custom graphical user interface built directly upon the Xlib/X11 API with an asynchronous execution engine. Terminator decouples the presentation layer from the execution engine, enabling two distinct process isolation models:
1. **Thread Mode (Default: `./terminator`)**: The entire emulator runs in a single process. Tabs operate as POSIX worker threads (`pthread_t`) that share application memory while synchronizing access to shared command history via POSIX semaphores (`sem_init(&g_shared_history->sem, 0, 1)`). Only the main GUI thread interacts with the X11 display socket.
2. **Process Mode (`./terminator useProcess=True`)**: The GUI runs inside the parent process, while each workspace tab runs as a dedicated child process created via `fork()`. Communication between the GUI and tab processes is maintained over non-blocking UNIX IPC pipes (`to_tab_fd` and `out_pipe_fd`). Command history is maintained in POSIX Shared Memory (`shm_open`, `ftruncate`, `mmap`) synchronized via a process-shared semaphore (`sem_init(&g_shared_history->sem, 1, 1)`).

The architecture multiplexes X11 display server events and child process I/O descriptors inside a central non-blocking `poll()` event loop, preventing UI freezes without busy-waiting.

```mermaid
flowchart TD
    subgraph Presentation_Layer ["Presentation Layer (X11 / Xlib)"]
        A["X11 Event Loop (gui.c:gui_run_loop)"]
        B["Double-Buffered Canvas (gui.c:gui_redraw)"]
        C["Hotkey & KeyPress Dispatcher (hotkeys.c / gui.c)"]
        D["Tab Workspace Manager (gui.c:gui_create_tab)"]
        A --> B
        A --> C
        A --> D
    end

    subgraph Parsing_Engine ["Lexical Analysis & AST Engine"]
        E["Tokenizer & Globber (parser.c:parse_line)"]
        F["Multiline State Machine (parser.c:is_command_incomplete)"]
        G["Variable & Subshell Expansion (parser.c:expand_variable)"]
        E --> F
        E --> G
    end

    subgraph Execution_Engine ["Execution Engine & Process Management"]
        H["Pipeline Orchestrator (shell.c:shell_execute_pipeline)"]
        I["Built-in Dispatcher (builtins.c:execute_builtin_captured)"]
        J["Process Group & Job Control (jobs.c:jobs_add / jobs_check_status)"]
        K["Parallel Execution Monitor (multiwatch.c:builtin_multiwatch)"]
        H --> I
        H --> J
        H --> K
    end

    subgraph IPC_Shared_State ["Synchronization & Shared Memory"]
        L["Thread/Process-Shared History (history.c:SharedHistory)"]
        M["UNIX IPC Pipes (dup2 / pipe)"]
    end

    Presentation_Layer -->|Command Line String| Parsing_Engine
    Parsing_Engine -->|Structured Pipeline| Execution_Engine
    Execution_Engine <-->|Shared Ring Buffer / Semaphores| IPC_Shared_State
    Execution_Engine -->|Asynchronous Pipe Output| Presentation_Layer
```

---

### 1.2 File & Module Inventory

| File Name | Header | Role & Responsibility | Core Functions & Key Structs |
| :--- | :--- | :--- | :--- |
| `src/main.c` | None | Primary process entry point; mode flag evaluation (Thread Mode vs Process Mode) and GUI lifecycle controller. | `main()` |
| `src/gui.c` | `include/gui.h` | X11 window management; double-buffering; tab lifecycle; input editing; event loop. | `gui_init()`, `gui_cleanup()`, `gui_run_loop()`, `gui_redraw()`, `gui_create_tab()`, `gui_close_tab()`, `gui_switch_tab()`, `tab_submit_command()`, `gui_handle_key_press()`, `gui_handle_button_press()`, `tab_process_main()`, `tab_worker_thread()`<br>Structs: `GuiApp`, `Tab`, `TermLine`, `LineType` |
| `src/parser.c` | `include/parser.h` | Lexer, tokenizer, multiline state machine, quote stripper, `$VAR` expander, command substitution, and wildcard globber. | `parse_line()`, `is_command_incomplete()`, `expand_variable()`, `execute_substitution()`, `flush_token()`, `free_pipeline()`, `free_command()`<br>Structs: `Command`, `Pipeline` |
| `src/shell.c` | `include/shell.h` | Pipeline orchestrator; I/O redirection; external process fork/exec; process group leader. | `shell_init()`, `shell_cleanup()`, `shell_execute_pipeline()`, `shell_execute_command()`, `setup_redirections()`<br>Structs: `ShellContext` |
| `src/builtins.c`| `include/builtins.h`| Built-in implementations affecting shell state; unified output capturing for GUI/pipes. | `is_builtin_command()`, `execute_builtin()`, `execute_builtin_captured()`, `builtin_cd()`, `builtin_exit()`, `builtin_clear()`, `builtin_jobs()`, `builtin_bg()`, `builtin_fg()`, `builtin_history()`, `builtin_source()` |
| `src/jobs.c` | `include/jobs.h` | POSIX job control table; process group tracking; asynchronous completion notifications. | `jobs_init()`, `jobs_add()`, `jobs_remove()`, `jobs_find_by_id()`, `jobs_find_by_pid()`, `jobs_check_status()`, `jobs_print()`, `jobs_builtin_bg()`, `jobs_builtin_fg()`, `jobs_kill_all()`, `jobs_get_current()`<br>Structs: `JobTable`, `Job`, `JobState` |
| `src/history.c` | `include/history.h` | Synchronized cross-tab command history ring buffer; POSIX shared memory; semaphores. | `history_init()`, `history_cleanup()`, `history_add()`, `history_get_relative()`, `history_count()`, `history_format_recent()`<br>Structs: `SharedHistory` |
| `src/multiwatch.c`| `include/multiwatch.h`| Concurrent parallel command execution; 2.0s refresh timer; formatted banner rendering. | `builtin_multiwatch()`, `parse_multiwatch_commands()`, `spawn_watched_cmd()`, `print_formatted_output()`<br>Structs: `WatchedCmd` |
| `src/hotkeys.c` | `include/hotkeys.h` | Keybinding configuration reader (`~/.terminatorrc`); conflict detector; X11 KeySym mapper. | `hotkeys_init()`, `hotkeys_match()`, `check_and_resolve_conflicts()`, `parse_combo_string()`<br>Structs: `HotkeyConfig`, `HotkeyBinding`, `HotkeyAction` |

---

## 2. Global Initialization & Bootstrap Flow

Before the user can type commands, Terminator initializes subsystems according to the selected mode.

```mermaid
sequenceDiagram
    autonumber
    participant Main as src/main.c:main()
    participant Hist as src/history.c:history_init()
    participant HK as src/hotkeys.c:hotkeys_init()
    participant GUI as src/gui.c:gui_init()
    participant Tab as src/gui.c:gui_create_tab()
    participant X11 as Xlib / X Server

    Main->>Main: Parse argv ("useProcess=True")
    Main->>Hist: history_init(use_process_mode)
    alt Thread Mode
        Hist->>Hist: malloc(sizeof(SharedHistory)) + sem_init(&sem, 0, 1)
    else Process Mode
        Hist->>Hist: shm_open("/terminator_shm_<pid>", O_CREAT|O_RDWR|O_EXCL, 0600) + ftruncate + mmap + sem_init(&sem, 1, 1)
    end
    Main->>HK: hotkeys_init(&dummy_hk) (~/.terminatorrc check)
    Main->>GUI: gui_init(&app, 900, 600, "Terminator", use_process_mode)
    GUI->>X11: XInitThreads() + XOpenDisplay(NULL)
    GUI->>X11: fcntl(ConnectionNumber(app->display), F_SETFD, FD_CLOEXEC)
    GUI->>X11: XCreateSimpleWindow() + GCs + XCreatePixmap()
    GUI->>X11: XInternAtom("WM_DELETE_WINDOW", "CLIPBOARD", "UTF8_STRING", "TARGETS", "TERMINATOR_SELECTION")
    GUI->>X11: XMapWindow() + XFlush()
    GUI->>Tab: gui_create_tab(&app, NULL)
    alt Thread Mode
        Tab->>Tab: pthread_create(&tab->worker_thread, NULL, tab_worker_thread, tab)
    else Process Mode
        Tab->>Tab: pipe(gui_to_tab) + pipe(tab_to_gui) + fork()
        Note over Tab: Child tab closes X11 socket fd directly and enters tab_process_main()
    end
    Main->>GUI: gui_run_loop(&app)
```

### 2.1 Detailed Call Chain Trace

1. **`main.c:main(argc, argv)`**:
   - Evaluates command-line flags: checks for `useProcess=True` to activate Process Mode (`use_process_mode = 1`), or defaults to Thread Mode (`use_process_mode = 0`).
   - Prints startup mode banner: `[Terminator] Mode: Thread Mode (default)` or `[Terminator] Mode: Process Mode (useProcess=True)`.
2. **`history.c:history_init(use_process_mode)`**:
   - In **Thread Mode** (`use_process_mode == 0`): Allocates heap memory `malloc(sizeof(SharedHistory))`. Initializes thread-shared POSIX semaphore `sem_init(&g_shared_history->sem, 0, 1)`.
   - In **Process Mode** (`use_process_mode == 1`): Formats unique shared memory path `/terminator_shm_<pid>` using `getpid()`. Calls `shm_unlink()` to clear any stale object, then `shm_open(shm_path, O_CREAT | O_RDWR | O_EXCL, 0600)`, truncates with `ftruncate(fd, sizeof(SharedHistory))`, maps with `mmap(NULL, sizeof(SharedHistory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)`, and initializes process-shared semaphore `sem_init(&g_shared_history->sem, 1, 1)`.
3. **`hotkeys.c:hotkeys_init(&cfg)`**:
   - Resolves `~/.terminatorrc`. If absent, writes default keybinding configuration via `write_default_config_file()`. Parses custom mappings, executes `check_and_resolve_conflicts()`, and falls back to default bindings if conflicts are found.
4. **GUI Initialization**:
   - Invokes `gui.c:gui_init(&app, 900, 600, "Terminator", use_process_mode)`. If the X server connection fails, reports error to stderr and exits with code 1.
5. **`gui.c:gui_init(app, width, height, title, use_process_mode)`**:
   - Calls `XInitThreads()`.
   - Establishes X11 display connection via `XOpenDisplay(NULL)` (falling back to `:0`).
   - Marks X11 connection socket with `FD_CLOEXEC` via `fcntl()` so child commands never inherit the X server descriptor.
   - Creates root simple window `XCreateSimpleWindow()`, loads monospace fonts, initializes double-buffering back buffer `Pixmap` via `XCreatePixmap()`.
   - Interns selection atoms (`WM_DELETE_WINDOW`, `CLIPBOARD`, `UTF8_STRING`, `TARGETS`, `TERMINATOR_SELECTION`).
   - Calls `XMapWindow()` and `XFlush()`.
   - Spawns Tab 1 via `gui.c:gui_create_tab(app, NULL)`.
6. **`gui.c:gui_create_tab(app, initial_cwd)`**:
   - Allocates `Tab` structure on heap. Initializes tab `ShellContext` and cwd.
   - If **Thread Mode**: sets `tab->is_thread_alive = 1` and calls `pthread_create(&tab->worker_thread, NULL, tab_worker_thread, tab)`.
   - If **Process Mode**: creates pipes `pipe(gui_to_tab)` and `pipe(tab_to_gui)`, calls `fork()`.
     - In Child Process: calls `setpgid(0, 0)`. Directly closes the child copy of the X11 connection socket descriptor (`close(ConnectionNumber(app->display))` and sets `app->display = NULL`). *Child MUST NOT call `XCloseDisplay()`*, because doing so sends protocol teardown packets over the socket that desynchronize XCB sequence numbers in the parent GUI event loop (`Assertion !xcb_xlib_threads_sequence_lost failed`). Child closes unused pipe ends and enters `tab_process_main(tab, gui_to_tab[0], tab_to_gui[1])`.
     - In Parent GUI Process: configures `tab->to_tab_fd = gui_to_tab[1]` and sets non-blocking mode on `tab->out_pipe_fd = tab_to_gui[0]` via `fcntl(O_NONBLOCK)`.

---

## 3. Task-by-Task / Command Execution Flows

---

### Task 1 — X11 GUI, Double-Buffering & Tab Multiplexing

#### 1. Trigger & Input
- Keystrokes inside X11 window (`KeyPress`), window resize (`ConfigureNotify`), window exposure (`Expose`), mouse clicks on tab bar / `[ + ]` button (`ButtonPress`), mouse wheel scroll (Button 4 / Button 5), or keyboard tab shortcuts (`Ctrl+T`, `Ctrl+Shift+W`, `Shift+Left`, `Shift+Right`).

#### 2. Step-by-Step Execution Trace
1. **`gui.c:gui_run_loop(GuiApp *app)`**:
   - Enters central while loop (`app->is_running`).
   - Drains pending X11 events via `while (XPending(app->display) > 0)`.
   - Dispatches `XNextEvent()`:
     - `Expose` $\rightarrow$ calls `gui.c:gui_redraw(app)`.
     - `ConfigureNotify` $\rightarrow$ updates `win_width`/`win_height`, re-allocates `app->back_buffer` Pixmap with `XCreatePixmap()`, calls `gui_redraw(app)`.
     - `KeyPress` $\rightarrow$ calls `gui.c:gui_handle_key_press(app, &ev.xkey)`.
     - `ButtonPress` $\rightarrow$ calls `gui.c:gui_handle_button_press(app, &ev.xbutton)`.
     - `ClientMessage` $\rightarrow$ checks `WM_DELETE_WINDOW`, sets `app->is_running = 0`.
2. **Multiplexing with Subprocess Pipes**:
   - Constructs `struct pollfd fds[]` array (capacity `MAX_POLL_ENTRIES = 1 + MAX_TABS * (1 + MAX_JOBS)`):
     - `fds[0].fd = ConnectionNumber(app->display)` (events = `POLLIN`).
     - For each tab: if `(t->is_running_cmd || t->use_process_mode) && t->out_pipe_fd >= 0`, adds `t->out_pipe_fd` (events = `POLLIN | POLLHUP`).
     - For each running background job: if `job->state == JOB_STATE_RUNNING && job->out_pipe_fd >= 0`, adds `job->out_pipe_fd`.
   - Executes `poll(fds, num_poll_fds, 50)` with a 50ms timeout to prevent busy-waiting.
3. **Reading Asynchronous Output**:
   - If `fds[i].revents & (POLLIN | POLLHUP)` triggers:
     - Invokes `read(fds[i].fd, buf, sizeof(buf))`.
     - If `buf` contains `[CWD]<path>\n`, extracts path into `tab->cwd` without printing and renders any remainder.
     - Otherwise invokes `gui.c:tab_append_output(tab, buf, n)`.
     - Sets `needs_redraw = 1`.
4. **Double-Buffered Rendering (`gui.c:gui_redraw`)**:
   - Fills background rectangle on `app->back_buffer` (`XFillRectangle`).
   - Renders tab buttons and anchored `[ + ]` button at `win_width - plus_w - 6`.
   - Computes visible scrollback row slice based on `tab->scroll_offset` and `tab->num_lines`.
   - Draws terminal text lines using `XDrawString()` into `app->back_buffer`.
   - Renders wrapped multiline user input buffer and block cursor (`XFillRectangle`).
   - Performs atomic blit to visible window via `XCopyArea(app->display, app->back_buffer, app->window, ...)` eliminating visual flicker.

```mermaid
flowchart TD
    A["poll() triggers (X11 event or Pipe data)"] --> B{Event Type?}
    B -->|X11 Event| C["gui_handle_key_press() / gui_handle_button_press()"]
    B -->|Pipe Data (POLLIN)| D["read(tab->out_pipe_fd)"]
    D --> E{Special [CWD] Tag?}
    E -->|Yes| F["Update tab->cwd for tab bar title"]
    E -->|No| G["tab_append_output() into tab->lines scrollback"]
    C --> H["Mark needs_redraw = 1"]
    G --> H
    H --> I["gui_redraw(): Paint into offscreen Pixmap"]
    I --> J["XCopyArea(): Blit Pixmap to X11 Window"]
```

---

### Task 2 — Built-in and External Command Execution

#### 1. Trigger & Input
- User presses <kbd>Return</kbd> on a single command (e.g., `ls -la`, `cd ~/Downloads`, `source script.sh`).

#### 2. Step-by-Step Execution Trace
1. **`gui.c:tab_submit_command(app, tab)`**:
   - Records prompt and input into scrollback buffer via `tab_record_prompt_input()`.
   - Checks multiline completeness using `parser.c:is_command_incomplete()`.
   - Appends finalized command to shared history via `history.c:history_add(cmd_str)`.
   - If **Process Mode**: writes command string to `tab->to_tab_fd` with `\n` and returns. The tab child process executes it inside `tab_process_main()`:
     - Built-ins run via `execute_builtin_captured()`; if `cd` succeeds, tab writes `[CWD]<path>\n` to `out_pipe_fd` to update the GUI tab bar.
     - External commands are forked with `dup2(out_fd, 1)` and `dup2(out_fd, 2)`.
   - If **Thread Mode**: passes `cmd_str` to `parser.c:parse_line()`.
2. **`parser.c:parse_line(cmd_str, ctx, pipeline)`**:
   - Splits tokens on whitespace, recognizing single quotes `'`, double quotes `"`, and redirections `<`, `>`.
   - Populates `Pipeline` containing an array of `Command` structs.
3. **Built-in Execution Path**:
   - Checks `builtins.c:is_builtin_command(cmd->argv[0])`.
   - If true: calls `builtins.c:execute_builtin_captured(&tab->shell_ctx, cmd, out_buf, sizeof(out_buf))`.
     - `cd`: calls `builtin_cd()` $\rightarrow$ expands `~` to `$HOME` $\rightarrow$ calls `chdir()` $\rightarrow$ calls `getcwd()` updating `ctx->cwd`.
     - `exit`: calls `gui_close_tab(app, app->active_tab_idx)`.
     - `clear`: wipes `tab->num_lines = 0; tab->scroll_offset = 0;`.
     - `jobs`: calls `jobs_print()` formatting background table into `out_buf`.
     - `bg`: calls `jobs_builtin_bg()` $\rightarrow$ sends `kill(-pgid, SIGCONT)` $\rightarrow$ sets state `Running`.
     - `fg`: calls `jobs_builtin_fg()` $\rightarrow$ sends `kill(-pgid, SIGCONT)` $\rightarrow$ connects pipe to foreground view in GUI.
     - `history`: calls `history.c:history_format_recent()` $\rightarrow$ dumps recent 1,000 commands.
     - `source`: calls `builtin_source()` $\rightarrow$ opens file via `fopen()` $\rightarrow$ executes each line in shell context.
     - `multiWatch`: dispatches parallel execution engine (`builtin_multiwatch`).
   - Appends `out_buf` to tab scrollback via `tab_append_output()`.
4. **External Command Execution Path (`gui.c:tab_submit_command` / `shell.c:shell_execute_command`)**:
   - Creates communication pipe `pipe(pipefds)`.
   - Calls `fork()`.
   - **Child Process (`pid == 0`)**:
     - Calls `setpgid(0, 0)` establishing a new process group.
     - Duplicates write-end to stdout and stderr: `dup2(pipefds[1], STDOUT_FILENO)` and `dup2(pipefds[1], STDERR_FILENO)`.
     - Closes both pipe ends: `close(pipefds[0]); close(pipefds[1]);`.
     - Applies redirections via `shell.c:setup_redirections()`.
     - Calls `execvp(cmd->argv[0], cmd->argv)`.
     - On failure: prints `terminator: <cmd>: command not found` to stderr and calls `_exit(127)`.
   - **Parent Process (`pid > 0`)**:
     - Closes write-end `close(pipefds[1])`.
     - Sets read-end `pipefds[0]` to non-blocking via `fcntl(O_NONBLOCK)`.
     - Sets `tab->running_pid = pid`, `tab->running_pgid = pid`, `tab->out_pipe_fd = pipefds[0]`, `tab->is_running_cmd = 1`.
     - Returns control immediately to `gui_run_loop()`, which reads output asynchronously via `poll()`.

```mermaid
sequenceDiagram
    autonumber
    participant GUI as gui.c:tab_submit_command
    participant Parser as parser.c:parse_line
    participant Builtin as builtins.c:execute_builtin_captured
    participant Shell as shell.c:shell_execute_command
    participant OS as Linux Kernel (fork/execvp)

    GUI->>Parser: parse_line(cmd_str, &pipeline)
    Parser-->>GUI: Pipeline populated
    alt Command is Built-in (cd, exit, jobs, etc.)
        GUI->>Builtin: execute_builtin_captured(ctx, cmd, out_buf, max)
        Builtin-->>GUI: Output formatted into out_buf
        GUI->>GUI: tab_append_output(tab, out_buf)
    else Command is External (ls, gcc, etc.)
        GUI->>Shell: pipe(pipefds) + fork()
        par Child Process
            Shell->>OS: setpgid(0, 0) + dup2(pipefds[1], 1)
            Shell->>OS: execvp(argv[0], argv)
        and Parent Process
            Shell->>GUI: tab->running_pid = pid, tab->out_pipe_fd = pipefds[0]
            Note over GUI: Output read asynchronously by poll() event loop
        end
    end
```

---

### Task 3 — Multiline Input, Quoting & Command Substitution

#### 1. Trigger & Input
- Unclosed quote (`echo "hello`), trailing backslash (`ls \`), trailing pipe (`cat file.txt |`), unclosed command substitution (`echo $(pwd)`), or backticks.

#### 2. Step-by-Step Execution Trace
1. **Continuation Detection (`parser.c:is_command_incomplete`)**:
   - Scans input character-by-character:
     - Maintains quote state flags: `in_sq` (single quote `'`), `in_dq` (double quote `"`), `in_bt` (backtick `` ` ``).
     - Maintains parenthesis nesting depth: `sub_depth` incremented on `$(` and decremented on `)`.
     - Detects trailing unescaped `\` or trailing `|`.
   - If any state flag is non-zero, returns `1`.
2. **GUI Prompt Transition**:
   - `tab_submit_command()` detects `is_command_incomplete() == 1`.
   - Sets `tab->is_multiline = 1`.
   - Switches active prompt from `user@terminator> ` to `> `.
   - Accumulates subsequent typed lines into `tab->multiline_accum` separated by `\n`.
3. **Variable & Substitution Expansion (`parser.c:expand_variable`)**:
   - When the command is complete, `parse_line()` tokenizes the buffer.
   - For unquoted or double-quoted segments:
     - `$VAR`: queries `getenv("VAR")` and substitutes value.
     - `$?`: formats `ctx->last_exit_status` as integer string.
     - `$(cmd)` or `` `cmd` ``: calls `parser.c:execute_substitution(sub_cmd, ctx, out_buf)`.
4. **Nested Command Substitution Execution (`parser.c:execute_substitution`)**:
   - Recursively executes inner subshell command before outer command:
     - Creates pipe `pipe(pipefds)`.
     - Calls `fork()`.
     - Child process: redirects stdout to `pipefds[1]` (`dup2`), parses inner command, executes via `shell_execute_pipeline()`, and calls `_exit()`.
     - Parent process: reads captured stdout from `pipefds[0]`, strips trailing POSIX newlines (`\r`/`\n`), and injects string into argument token.

---

### Task 4 — I/O Redirection

#### 1. Trigger & Input
- Command tokens containing `< input_file`, `> output_file`, or `./prog < in.txt > out.txt`.

#### 2. Step-by-Step Execution Trace
1. **Parsing Redirections (`parser.c:parse_single_command`)**:
   - Detects `<` operator: extracts filename into `cmd->input_file`.
   - Detects `>` operator: extracts filename into `cmd->output_file`.
   - If wildcard expansion on redirection target yields multiple files: reports `terminator: <token>: ambiguous redirect` and returns `-1`.
   - Strips redirection tokens from `cmd->argv` so arguments are clean for `execvp()`.
2. **Applying Redirections in Child Process (`shell.c:setup_redirections`)**:
   - For `input_file`:
     - Calls `open(input_file, O_RDONLY)`.
     - If file does not exist (`in_fd < 0`): prints error `terminator: <file>: <strerror>` to stderr and halts execution immediately (`_exit(1)`).
     - Calls `dup2(in_fd, STDIN_FILENO)` and `close(in_fd)`.
   - For `output_file`:
     - Calls `open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644)`.
     - Calls `dup2(out_fd, STDOUT_FILENO)` and `close(out_fd)`.
3. **Built-in Redirection Handling (`shell.c:shell_execute_command`)**:
   - In subshells or scripted pipelines, if a built-in has redirections, Terminator uses `dup()` to save original stdin/stdout, applies redirections, runs the built-in, and restores original descriptors via `dup2()`.

```mermaid
flowchart LR
    A["Child Process (fork)"] --> B{input_file set?}
    B -->|Yes| C["open(input_file, O_RDONLY)"]
    C -->|Success| D["dup2(in_fd, STDIN_FILENO) & close(in_fd)"]
    C -->|Failure| E["perror() & _exit(1)"]
    B -->|No| F{output_file set?}
    D --> F
    F -->|Yes| G["open(output_file, O_WRONLY|O_CREAT|O_TRUNC, 0644)"]
    G --> H["dup2(out_fd, STDOUT_FILENO) & close(out_fd)"]
    F -->|No| I["execvp(argv[0], argv)"]
    H --> I
```

---

### Task 5 — Pipelines & Pathname Globbing

#### 1. Trigger & Input
- Pipeline of $N$ commands: `cat file.txt | grep error | wc -l`, or wildcard arguments: `ls *.c`, `rm ~/tmp/*.o`.

#### 2. Step-by-Step Execution Trace
1. **Pathname Expansion / Globbing (`parser.c:flush_token`)**:
   - Before adding an unquoted token to `cmd->argv`, tests for wildcard characters (`*`, `?`, `[`, `~`).
   - If present: invokes `glob(token, GLOB_NOCHECK | GLOB_TILDE, NULL, &glob_buf)`.
   - Appends all matched paths in alphabetical order into `cmd->argv`.
   - Calls `globfree(&glob_buf)`. Quoted wildcards (e.g. `"*.c"`) are preserved literally.
2. **Pipeline Plumbing (`shell.c:shell_execute_pipeline`)**:
   - For $N$ commands, allocates `int *pipefds = malloc(sizeof(int) * 2 * (N - 1))`.
   - Loops $i$ from 0 to $N-1$:
     - If $i < N - 1$, calls `pipe(pipefds + (i * 2))`.
     - Calls `fork()`.
     - In Child $i$:
       - If $i == 0$: sets process group leader via `setpgid(0, 0)`.
       - If $i > 0$: joins leader's group via `setpgid(0, pids[0])` and attaches stdin: `dup2(pipefds[(i - 1) * 2], STDIN_FILENO)`.
       - If $i < N - 1$: attaches stdout: `dup2(pipefds[i * 2 + 1], STDOUT_FILENO)`.
       - Applies any overriding file redirections (`<` or `>`).
       - Closes all $2(N-1)$ pipe descriptors in child.
       - Invokes `execvp(cmd->argv[0], cmd->argv)` or executes built-in.
   - In Parent:
     - Closes all $2(N-1)$ pipe descriptors immediately so reader processes receive `EOF` when writers terminate.
     - Waits for all $N$ child processes using `waitpid()`.
     - Captures exit status of the rightmost command ($N-1$) into `ctx->last_exit_status`.
3. **Background Pipelines (`pipeline->is_background`)**:
   - Forks background supervisor process. If stdin is unredirected, redirects stdin from `/dev/null`.
   - Registers job into `tab->shell_ctx.job_table`, prints `[id] pid`, and returns immediately.

```mermaid
flowchart LR
    subgraph Pipeline_Execution ["N Commands, N-1 Pipes"]
        C1["Child 0 (cat)"] -->|pipefds[0]| C2["Child 1 (grep)"]
        C2 -->|pipefds[1]| C3["Child 2 (wc)"]
    end
    Parent["Parent Process"] -.->|Closes all pipe ends & waits| C3
```

---

### Task 6 — `multiWatch`

#### 1. Trigger & Input
- Command syntax: `multiWatch ["date +%T", "ls | wc -l"]`.

#### 2. Step-by-Step Execution Trace
1. **Argument Parsing (`multiwatch.c:parse_multiwatch_commands`)**:
   - Extracts command strings from brackets `[...]` and comma-separated lists. Strips outer quotes and trims whitespace into `WatchedCmd` structures.
2. **Signal Handling Configuration**:
   - Registers dedicated signal handler `mw_sigint_handler()` on `SIGINT` (`Ctrl+C`), setting `g_mw_interrupted = 1`.
3. **Execution & Refresh Engine (`multiwatch.c:builtin_multiwatch`)**:
   - Spawns all watched commands initially in parallel via `spawn_watched_cmd()`, wiring non-blocking pipes to each child.
   - Loops while `!g_mw_interrupted`:
     - Checks timestamp `now = time(NULL)`. For any command with `pid == 0` whose elapsed time `now - last_finish_time >= 2`, respawns it automatically.
     - Gathers read-ends into `struct pollfd pfds[]`.
     - Calls `poll(pfds, num_active, 100)` with a 100ms timeout slice.
     - When `POLLIN` fires: reads output chunk into `cmd->output_buf`.
     - When child finishes (`waitpid(cmd->pid, &status, WNOHANG) > 0`):
       - Drains remaining bytes from pipe, closes pipe descriptor.
       - Records finish timestamp `cmd->last_finish_time = time(NULL)`.
       - Calls `print_formatted_output()`:
         ```text
         "cmd", <timestamp>:
         ----------------------------------------------------
         <output>
         ----------------------------------------------------
         ```
       - Resets `output_buf` for subsequent execution cycles.
4. **Interruption & Teardown**:
   - When user presses `Ctrl+C`: `g_mw_interrupted` flag becomes 1.
   - Iterates through active commands, sending `kill(-cmd->pid, SIGTERM)` and `kill(cmd->pid, SIGTERM)`.
   - Closes open pipes and reaps processes via `waitpid(cmd->pid, NULL, WNOHANG)`.
   - Restores original `sigaction(SIGINT)` handler and returns to prompt.

---

### Task 7 — Hotkeys & Dynamic Configuration

#### 1. Trigger & Input
- Key combinations in GUI: `Ctrl+A`, `Ctrl+E`, `Ctrl+U`, `Ctrl+K`, `Ctrl+W`, `Ctrl+L`, `Ctrl+D`, `Ctrl+Shift+C`, `Ctrl+Shift+V`, `Up`, `Down`, as well as tab shortcuts (`Ctrl+T`, `Ctrl+Shift+W`, `Shift+Left`, `Shift+Right`).

#### 2. Step-by-Step Execution Trace
1. **Config Initialization (`hotkeys.c:hotkeys_init`)**:
   - Reads `~/.terminatorrc` at startup. If absent, writes default template.
   - Parses binding strings into X11 modifier bitmasks (`ControlMask`, `ShiftMask`, `Mod1Mask`) and KeySyms via `XStringToKeysym()`.
   - Runs `check_and_resolve_conflicts()`: if duplicate bindings exist, reverts conflicting actions to defaults with a warning.
2. **Matching & Execution (`gui.c:gui_handle_key_press`)**:
   - Calls `hotkeys.c:hotkeys_match(&app->hotkeys, event, keysym)`.
   - Dispatches matched action:
     - `HOTKEY_MOVE_START`: sets `tab->cursor_pos = 0`.
     - `HOTKEY_MOVE_END`: sets `tab->cursor_pos = tab->input_len`.
     - `HOTKEY_DELETE_TO_START`: deletes slice `[0, cursor_pos]` via `tab_delete_range()`.
     - `HOTKEY_DELETE_TO_END`: deletes slice `[cursor_pos, input_len]`.
     - `HOTKEY_DELETE_WORD`: steps backward past spaces then word characters, deleting range.
     - `HOTKEY_CLEAR_SCREEN`: resets `tab->num_lines = 0; tab->scroll_offset = 0;`.
     - `HOTKEY_EXIT_SHELL`: if input empty, closes tab via `gui_close_tab()`.
     - `HOTKEY_COPY`: copies `tab->input_buffer` to `app->clipboard_data` and asserts X11 ownership of `CLIPBOARD` and `XA_PRIMARY` atoms (`XSetSelectionOwner`).
     - `HOTKEY_PASTE`: requests selection transfer from clipboard via `XConvertSelection()`. Output arrives as `SelectionNotify` event in `gui_run_loop()` and inserts text at cursor.
     - `HOTKEY_HISTORY_PREV`: if input is wrapped across lines and cursor is not on the top row, moves cursor up one line; otherwise browses older history via `history_get_relative(tab->history_pos++)`.
     - `HOTKEY_HISTORY_NEXT`: if input is wrapped across lines and cursor is not on the bottom row, moves cursor down one line; otherwise moves toward newer history, restoring in-progress typing from `tab->saved_input` when returning to the bottom.

---

### Task 8 — Signals & Job Control

#### 1. Trigger & Input
- Keystrokes `Ctrl+C`, `Ctrl+Z`, command suffix `&` (`sleep 100 &`), or built-ins `jobs`, `fg %id`, `bg %id`.

#### 2. Step-by-Step Execution Trace
1. **Foreground Signal Delivery (`gui.c:gui_handle_key_press`)**:
   - X11 intercepts keystrokes as `KeyPress` events. Terminator converts them into OS signals:
     - In **Process Mode**:
       - `Ctrl+C`: sends `kill(-tab->tab_pid, SIGINT)` directly to the dedicated tab child process group.
       - `Ctrl+Z`: sends `kill(-tab->tab_pid, SIGTSTP)` directly to the dedicated tab child process group.
     - In **Thread Mode**:
       - `Ctrl+C`: if foreground command running, sends `kill(-tab->running_pgid, SIGINT)` and `kill(tab->running_pid, SIGINT)`. Terminator itself never terminates.
       - `Ctrl+Z`: sends `kill(-tab->running_pgid, SIGTSTP)` and `kill(tab->running_pid, SIGTSTP)`.
2. **Suspending into Job Table (`gui.c:gui_handle_key_press`)**:
   - In Thread Mode on `Ctrl+Z`, registers stopped process group in table:
     `jobs_add(&tab->shell_ctx.job_table, pgid, pid, cmd, JOB_STATE_STOPPED, pipe_fd)`.
   - Prints status notice: `[^Z\n[1]+  Stopped                 <cmd>]`.
   - Clears `tab->is_running_cmd = 0`, freeing prompt for input.
3. **Background Execution (`cmd &`)**:
   - `parser.c:parse_line()` detects trailing `&`, sets `pipeline->is_background = 1`.
   - Spawns process group in background.
   - Registers job in `tab->shell_ctx.job_table`, sets `tab->is_running_cmd = 0`, and prints launch notice `[1] <pid>`. Prompt returns immediately.
4. **Resuming Jobs (`bg` and `fg`)**:
   - `bg %id`: resolves Job entry via `jobs_find_by_id()`, broadcasts `kill(-job->pgid, SIGCONT)`, updates state to `JOB_STATE_RUNNING`, and prints `[id]+ <cmd> &`.
   - `fg %id`: resolves Job entry via `jobs_builtin_fg()`, broadcasts `kill(-job->pgid, SIGCONT)`, removes from background table, and re-attaches `job->out_pipe_fd` to foreground view in GUI.
5. **Asynchronous Completion Notices (`jobs_check_status`)**:
   - Inside `gui_run_loop()`, each poll cycle invokes `jobs_check_status(&tab->shell_ctx.job_table, notif_buf, sizeof(notif_buf))`.
   - Calls `waitpid(j->pid, &status, WNOHANG | WUNTRACED | WCONTINUED)`.
   - If a background job terminated, formats notice: `[id]+  Done                    <cmd>\n`, removes job from table, and prints notice into active tab scrollback.

```mermaid
sequenceDiagram
    autonumber
    participant User as User / X11 Window
    participant GUI as gui.c:gui_handle_key_press
    participant Jobs as jobs.c:jobs_add
    participant OS as Linux Kernel (kill/waitpid)

    User->>GUI: Presses Ctrl+Z while sleep 100 is running
    GUI->>OS: kill(-running_pgid, SIGTSTP)
    GUI->>Jobs: jobs_add(&job_table, pgid, pid, "sleep 100", JOB_STATE_STOPPED, fd)
    Jobs-->>GUI: Job ID 1 assigned
    GUI->>User: Displays "[1]+  Stopped  sleep 100" and restores prompt
    User->>GUI: Types "bg %1"
    GUI->>OS: kill(-job->pgid, SIGCONT)
    OS-->>GUI: Process continues running in background
    User->>GUI: User continues typing commands...
    Note over GUI,OS: Background job finishes
    GUI->>OS: waitpid(-1, &status, WNOHANG)
    OS-->>GUI: Child terminated (WIFEXITED)
    GUI->>User: Asynchronous notice: "[1]+  Done  sleep 100"
```

---

### Task 9 — Shared Command History

#### 1. Trigger & Input
- Every command executed via <kbd>Return</kbd>, Up/Down arrow keys, or `history` built-in command.

#### 2. Step-by-Step Execution Trace
1. **Appending Command (`history.c:history_add`)**:
   - Acquires semaphore: `sem_wait(&g_shared_history->sem)`.
   - Checks ring buffer bounds: if input is identical to most recent entry or empty, ignores it.
   - Copies string to `g_shared_history->commands[g_shared_history->head]`.
   - Strips trailing newlines.
   - Increments `head = (head + 1) % MAX_HISTORY_COMMANDS`.
   - Increments `count` up to `MAX_HISTORY_COMMANDS` (10,000).
   - Releases semaphore: `sem_post(&g_shared_history->sem)`.
2. **Relative History Lookup (`history.c:history_get_relative`)**:
   - Used by <kbd>Up</kbd> (`offset = 0, 1, ...`) and <kbd>Down</kbd> arrow keys.
   - Acquires semaphore: `sem_wait(&g_shared_history->sem)`.
   - Computes circular ring index:
     `idx = (head - 1 - offset + 2 * MAX_HISTORY_COMMANDS) % MAX_HISTORY_COMMANDS`.
   - Copies command string to output buffer.
   - Releases semaphore: `sem_post(&g_shared_history->sem)`.
3. **History Command Formatting (`history.c:history_format_recent`)**:
   - `builtin_history()` requests recent 1,000 entries.
   - Iterates through valid ring buffer slots, formatting numbered list:
     ```text
         1  echo first_command
         2  echo second_command
         3  history
     ```
   - Emits buffer to stdout or tab scrollback.

---

## 4. Teardown & Cleanup Flow

Terminator provides leak-free teardown across both execution models.

```mermaid
sequenceDiagram
    autonumber
    participant App as Main Process
    participant GUI as gui.c:gui_cleanup()
    participant Tab as Tabs (Threads / Child Processes)
    participant Hist as history.c:history_cleanup()
    participant OS as Operating System / X Server

    App->>GUI: gui_cleanup(&app)
    loop For Each Active Tab
        alt Thread Mode
            GUI->>Tab: Set is_thread_alive = 0 + pthread_cancel() + pthread_join()
        else Process Mode
            GUI->>Tab: kill(-tab_pid, SIGTERM) + kill(tab_pid, SIGTERM) + waitpid()
        end
        GUI->>OS: close(tab->out_pipe_fd) + close(tab->to_tab_fd)
        GUI->>Tab: jobs_kill_all(&tab->shell_ctx.job_table)
        GUI->>Tab: free(tab)
    end
    GUI->>OS: XFreeGC() (10 GCs) + XFreeFont() + XFreePixmap() + XDestroyWindow()
    GUI->>OS: XCloseDisplay(app->display)
    App->>Hist: history_cleanup()
    alt Thread Mode
        Hist->>OS: sem_destroy(&sem) + free(g_shared_history)
    else Process Mode
        Hist->>OS: sem_destroy(&sem) + munmap() + shm_unlink("/terminator_shm_<pid>")
    end
```

### 4.1 Teardown Execution Steps

1. **Window Closure or Exit Command**:
   - Triggered by `ClientMessage` (`WM_DELETE_WINDOW`), `exit` built-in, or `Ctrl+D` on an empty prompt.
   - Sets `app->is_running = 0`, exiting `gui_run_loop()`.
2. **Tab Teardown (`gui.c:gui_cleanup`)**:
   - Iterates through open tabs:
     - Closes IPC pipe descriptors: `close(tab->out_pipe_fd)` and `close(tab->to_tab_fd)`.
     - In **Thread Mode**: sets `tab->is_thread_alive = 0`, invokes `pthread_cancel(tab->worker_thread)`, and blocks on `pthread_join(tab->worker_thread, NULL)`. If a foreground command is executing, signals it with `kill(-running_pgid, SIGTERM)`.
     - In **Process Mode**: broadcasts termination signals to the tab child process group via `kill(-tab->tab_pid, SIGTERM)` and `kill(tab->tab_pid, SIGTERM)`, and reaps with `waitpid(tab->tab_pid, NULL, WNOHANG)`.
     - Broadcasts `SIGTERM`, `SIGKILL`, and `SIGCONT` to all active background jobs via `jobs_kill_all(&tab->shell_ctx.job_table)`.
     - Deallocates the `Tab` structure via `free(tab)`.
3. **X11 Resource Deallocation**:
   - Frees Graphics Contexts: `XFreeGC(app->display, gc)` for all 10 allocated GCs (`gc_bg`, `gc_prompt`, `gc_input`, `gc_output`, `gc_cursor`, `gc_tab_bar_bg`, `gc_tab_active_bg`, `gc_tab_text`, `gc_tab_active_text`, `gc_scrollbar`).
   - Frees loaded monospace font struct: `XFreeFont(app->display, app->font)`.
   - Frees offscreen Pixmap buffer: `XFreePixmap(app->display, app->back_buffer)`.
   - Destroys top-level window: `XDestroyWindow(app->display, app->window)`.
   - Closes X11 display socket connection: `XCloseDisplay(app->display)`.
4. **Shared History Cleanup (`history.c:history_cleanup`)**:
   - Destroys POSIX semaphore via `sem_destroy(&g_shared_history->sem)`.
   - In **Thread Mode**: deallocates heap segment via `free(g_shared_history)`.
   - In **Process Mode**: unmaps shared memory segment via `munmap(g_shared_history, sizeof(SharedHistory))` and unlinks the named shared memory segment from `/dev/shm` via `shm_unlink(g_shm_name)`.

---

## 5. Cross-Reference Summary Table

| Task / Command | Entry Function (`File:Func`) | Core Worker Functions (`File:Func`) | Key Data Structures Modified | Helper / Utility Files Used |
| :--- | :--- | :--- | :--- | :--- |
| **System Startup** | `src/main.c:main` | `history_init`, `gui_init`, `hotkeys_init`, `gui_create_tab` | `GuiApp`, `SharedHistory`, `HotkeyConfig` | `history.c`, `gui.c`, `hotkeys.c` |
| **X11 Event Loop** | `src/gui.c:gui_run_loop` | `poll`, `gui_redraw`, `gui_handle_key_press`, `tab_append_output` | `GuiApp`, `struct pollfd[]` | `gui.c`, `Xlib` |
| **Double-Buffered Draw**| `src/gui.c:gui_redraw` | `XFillRectangle`, `XDrawString`, `XCopyArea` | `app->back_buffer`, `tab->lines` | `gui.c`, `Xlib` |
| **Tab Creation** | `src/gui.c:gui_create_tab` | `pthread_create` (Thread) / `pipe` + `fork` (Process) | `app->tabs[]`, `app->num_tabs`, `Tab` | `gui.c`, `pthread`, `unistd` |
| **Tab Destruction** | `src/gui.c:gui_close_tab` | `pthread_cancel` / `kill(SIGTERM)`, `close`, `free` | `app->tabs[]`, `app->num_tabs` | `gui.c`, `jobs.c` |
| **Built-in Dispatch**| `src/builtins.c:execute_builtin_captured` | `builtin_cd`, `builtin_exit`, `builtin_jobs`, `builtin_source` | `ctx->cwd`, `ctx->should_exit`, `ctx->last_exit_status` | `builtins.c`, `jobs.c` |
| **External Command** | `src/shell.c:shell_execute_command` | `fork`, `setpgid`, `dup2`, `setup_redirections`, `execvp` | `ctx->foreground_pid`, `tab->running_pid` | `shell.c`, `unistd` |
| **Pipeline ($N$ Pipes)**| `src/shell.c:shell_execute_pipeline` | `pipe`, `fork`, `dup2`, `close`, `setpgid`, `waitpid` | `Pipeline`, `Command`, `ctx->last_exit_status` | `shell.c`, `parser.c` |
| **Multiline & Quote**| `src/parser.c:parse_line`| `is_command_incomplete`, `expand_variable`, `execute_substitution`| `tab->multiline_accum`, `tab->is_multiline` | `parser.c` |
| **Globbing Expansion**| `src/parser.c:flush_token`| `glob`, `globfree` | `cmd->argv[]`, `cmd->argc` | `parser.c`, `<glob.h>` |
| **`multiWatch`** | `src/multiwatch.c:builtin_multiwatch` | `parse_multiwatch_commands`, `spawn_watched_cmd`, `poll` | `WatchedCmd[]`, `g_mw_interrupted` | `multiwatch.c`, `time.h` |
| **Hotkey Handling** | `src/gui.c:gui_handle_key_press` | `hotkeys_match`, `tab_delete_range`, `tab_insert_char` | `tab->input_buffer`, `tab->cursor_pos` | `gui.c`, `hotkeys.c` |
| **Signal Delivery** | `src/gui.c:gui_handle_key_press` | `kill(-pgid, SIGINT)`, `kill(-pgid, SIGTSTP)` | `tab->running_pid`, `tab->is_running_cmd` | `gui.c`, `signal.h` |
| **Background Job** | `src/shell.c:shell_execute_pipeline` | `fork`, `setpgid`, `jobs_add` | `ctx->job_table.jobs[]`, `Job` | `shell.c`, `jobs.c` |
| **Job Resumption** | `src/jobs.c:jobs_builtin_bg` | `jobs_find_by_id`, `kill(SIGCONT)` | `Job.state`, `job_table.current_job_id` | `jobs.c`, `builtins.c` |
| **Shared History** | `src/history.c:history_add`| `sem_wait`, `sem_post` | `SharedHistory.commands[][]`, `head`, `count` | `history.c`, `semaphore.h` |
| **System Teardown** | `src/gui.c:gui_cleanup` | `XFreeGC`, `XFreePixmap`, `XCloseDisplay`, `history_cleanup` | `GuiApp`, `SharedHistory` | `gui.c`, `history.c` |
