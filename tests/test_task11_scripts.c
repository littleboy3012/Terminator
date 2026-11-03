#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "shell.h"
#include "parser.h"
#include "builtins.h"
#include "script.h"
#include "history.h"

static void create_file(const char *path, const char *content, mode_t mode) {
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
    chmod(path, mode);
}

static void test_script_validation(void) {
    printf("  [11.1] Testing Syntax Validation (balanced if...fi)...\n");
    ShellContext ctx;
    shell_init(&ctx);

    /* Unbalanced if (missing fi) */
    create_file("/tmp/bad_script1.sh", "if [ 1 -eq 1 ]; then\n echo yes\n", 0755);
    int rc = script_run(&ctx, "/tmp/bad_script1.sh", 0, NULL);
    assert(rc != 0);
    unlink("/tmp/bad_script1.sh");

    /* Stray fi without if */
    create_file("/tmp/bad_script2.sh", "echo hello\nfi\n", 0755);
    rc = script_run(&ctx, "/tmp/bad_script2.sh", 0, NULL);
    assert(rc != 0);
    unlink("/tmp/bad_script2.sh");

    printf("    Syntax validation passed!\n");
}

static void test_script_variables_and_posargs(void) {
    printf("  [11.2] Testing Script Variables & Positional Arguments ($0, $#, $1..$9)...\n");
    ShellContext ctx;
    shell_init(&ctx);

    const char *script_text =
        "#!/bin/bash\n"
        "# Comment line\n"
        "MY_VAR=terminator\n"
        "echo \"var=$MY_VAR\"\n"
        "echo \"arg0=$0\"\n"
        "echo \"argc=$#\"\n"
        "echo \"arg1=$1\"\n"
        "echo \"arg2=$2\"\n";

    create_file("/tmp/pos_script.sh", script_text, 0755);

    char *argv[] = { "pos_script.sh", "first", "second_arg", NULL };
    int rc = script_run(&ctx, "/tmp/pos_script.sh", 3, argv);
    assert(rc == 0);
    assert(ctx.last_exit_status == 0);

    unlink("/tmp/pos_script.sh");
    printf("    Variables and positional arguments passed!\n");
}

static void test_script_conditionals(void) {
    printf("  [11.3] Testing Script Conditionals (if, elif, else, [ ... ] tests)...\n");
    ShellContext ctx;
    shell_init(&ctx);

    create_file("/tmp/test_target.txt", "sample content\n", 0644);

    const char *cond_script =
        "#!/bin/bash\n"
        "TARGET=/tmp/test_target.txt\n"
        "if [ -f \"$TARGET\" ]; then\n"
        "    echo \"file exists\"\n"
        "    RES=1\n"
        "else\n"
        "    echo \"file missing\"\n"
        "    RES=0\n"
        "fi\n"
        "\n"
        "if [ ! -d \"$TARGET\" ]; then\n"
        "    echo \"not a directory\"\n"
        "fi\n"
        "\n"
        "NUM=42\n"
        "if [ $NUM -lt 10 ]; then\n"
        "    VAL=low\n"
        "elif [ $NUM -eq 42 ]; then\n"
        "    VAL=correct\n"
        "else\n"
        "    VAL=high\n"
        "fi\n"
        "exit 5\n";

    create_file("/tmp/cond_script.sh", cond_script, 0755);

    int rc = script_run(&ctx, "/tmp/cond_script.sh", 0, NULL);
    assert(rc == 5);
    assert(ctx.last_exit_status == 5);

    unlink("/tmp/test_target.txt");
    unlink("/tmp/cond_script.sh");
    printf("    Conditionals and exit codes passed!\n");
}

static void test_script_pipelines_and_redirection(void) {
    printf("  [11.4] Testing Script Pipelines and Redirections...\n");
    ShellContext ctx;
    shell_init(&ctx);

    const char *pipe_script =
        "#!/bin/bash\n"
        "echo \"apple banana cherry\" | tr ' ' '\\n' | sort | head -2 > /tmp/script_out.txt\n";

    create_file("/tmp/pipe_script.sh", pipe_script, 0755);

    int rc = script_run(&ctx, "/tmp/pipe_script.sh", 0, NULL);
    assert(rc == 0);

    FILE *f = fopen("/tmp/script_out.txt", "r");
    assert(f != NULL);
    char buf[128];
    assert(fgets(buf, sizeof(buf), f) != NULL);
    assert(strcmp(buf, "apple\n") == 0);
    assert(fgets(buf, sizeof(buf), f) != NULL);
    assert(strcmp(buf, "banana\n") == 0);
    fclose(f);

    unlink("/tmp/pipe_script.sh");
    unlink("/tmp/script_out.txt");
    printf("    Pipelines and redirections inside scripts passed!\n");
}

static void test_script_source_and_interception(void) {
    printf("  [11.5] Testing source / . built-in and shell execution interception...\n");
    ShellContext ctx;
    shell_init(&ctx);

    const char *src_script =
        "SOURCED_VAR=sourced_value\n";
    create_file("/tmp/source_test.sh", src_script, 0644);

    Pipeline pl;
    int rc = parse_line("source /tmp/source_test.sh", &ctx, &pl);
    assert(rc == 0);
    assert(pl.num_commands == 1);
    rc = builtin_source(&ctx, pl.commands[0].argv);
    assert(rc == 0);
    free_pipeline(&pl);

    /* Verify SOURCED_VAR persisted into environment */
    const char *val = getenv("SOURCED_VAR");
    assert(val != NULL);
    assert(strcmp(val, "sourced_value") == 0);

    unlink("/tmp/source_test.sh");
    printf("    source and variable persistence passed!\n");
}

int main(void) {
    printf("=== Task 11 Bash Script Interpreter Unit Tests ===\n");
    test_script_validation();
    test_script_variables_and_posargs();
    test_script_conditionals();
    test_script_pipelines_and_redirection();
    test_script_source_and_interception();
    printf("=== ALL TASK 11 TESTS PASSED SUCCESSFULLY! ===\n");
    return 0;
}
