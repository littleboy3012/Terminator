#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "history.h"

/* Mock per-tab worker in thread mode */
static void *thread_worker(void *arg) {
    long id = (long)arg;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "thread_cmd_%ld", id);
    history_add(cmd);
    return NULL;
}

static void test_thread_mode(void) {
    printf("=== Testing Thread Mode Concurrency & Shared History ===\n");
    assert(history_init(0) == 0);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread_worker, (void*)1);
    pthread_create(&t2, NULL, thread_worker, (void*)2);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    assert(history_count() >= 2);
    char buf[128];
    assert(history_get_relative(1, buf, sizeof(buf)) == 0);
    printf("Most recent history in Thread Mode: %s\n", buf);

    history_cleanup();
    printf(">>> Thread Mode Test PASSED!\n\n");
}

static void test_process_mode(void) {
    printf("=== Testing Process Mode Concurrency & POSIX Shared Memory History ===\n");
    assert(history_init(1) == 0);

    pid_t p1 = fork();
    if (p1 == 0) {
        history_add("process_tab1_cmd");
        _exit(0);
    }

    pid_t p2 = fork();
    if (p2 == 0) {
        history_add("process_tab2_cmd");
        _exit(0);
    }

    int status;
    waitpid(p1, &status, 0);
    waitpid(p2, &status, 0);

    assert(history_count() >= 2);
    char buf[128];
    assert(history_get_relative(1, buf, sizeof(buf)) == 0);
    printf("Most recent history in Process Mode: %s\n", buf);

    history_cleanup();
    printf(">>> Process Mode Test PASSED!\n\n");
}

int main(void) {
    test_thread_mode();
    test_process_mode();
    printf("ALL CONCURRENCY AND MODE TESTS COMPLETED SUCCESSFULLY!\n");
    return 0;
}
