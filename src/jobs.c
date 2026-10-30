#include "jobs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <sys/wait.h>

void jobs_init(JobTable *table) {
    if (!table) return;
    memset(table, 0, sizeof(JobTable));
    for (int i = 0; i < MAX_JOBS; i++) {
        table->jobs[i].out_pipe_fd = -1;
    }
    table->num_jobs = 0;
    table->current_job_id = 0;
    table->previous_job_id = 0;
}

Job *jobs_add(JobTable *table, pid_t pgid, pid_t pid, const char *cmd, JobState state, int pipe_fd) {
    if (!table || table->num_jobs >= MAX_JOBS) {
        return NULL;
    }

    /* Assign lowest available positive integer ID >= 1 */
    int assigned_id = 1;
    while (1) {
        int taken = 0;
        for (int i = 0; i < table->num_jobs; i++) {
            if (table->jobs[i].id == assigned_id) {
                taken = 1;
                break;
            }
        }
        if (!taken) break;
        assigned_id++;
    }

    int idx = table->num_jobs;
    Job *j = &table->jobs[idx];
    memset(j, 0, sizeof(Job));
    j->id = assigned_id;
    j->pgid = pgid;
    j->pid = pid;
    j->state = state;
    j->out_pipe_fd = pipe_fd;
    j->exit_status = 0;

    if (cmd) {
        snprintf(j->command, sizeof(j->command), "%s", cmd);
        /* Trim trailing '&' and spaces from recorded command text */
        int len = (int)strlen(j->command);
        while (len > 0 && (isspace((unsigned char)j->command[len - 1]) || j->command[len - 1] == '&')) {
            j->command[len - 1] = '\0';
            len--;
        }
    } else {
        j->command[0] = '\0';
    }

    table->num_jobs++;

    /* Update current and previous job references */
    table->previous_job_id = table->current_job_id;
    table->current_job_id = assigned_id;

    return j;
}

Job *jobs_find_by_id(JobTable *table, int id) {
    if (!table || id <= 0) return NULL;
    for (int i = 0; i < table->num_jobs; i++) {
        if (table->jobs[i].id == id) {
            return &table->jobs[i];
        }
    }
    return NULL;
}

Job *jobs_find_by_pid(JobTable *table, pid_t pid) {
    if (!table || pid <= 0) return NULL;
    for (int i = 0; i < table->num_jobs; i++) {
        if (table->jobs[i].pid == pid || table->jobs[i].pgid == pid) {
            return &table->jobs[i];
        }
    }
    return NULL;
}

Job *jobs_get_current(JobTable *table) {
    if (!table || table->num_jobs == 0) return NULL;
    Job *j = jobs_find_by_id(table, table->current_job_id);
    if (j) return j;
    return &table->jobs[table->num_jobs - 1];
}

int jobs_remove(JobTable *table, int id) {
    if (!table || id <= 0) return -1;

    int found_idx = -1;
    for (int i = 0; i < table->num_jobs; i++) {
        if (table->jobs[i].id == id) {
            found_idx = i;
            break;
        }
    }

    if (found_idx < 0) return -1;

    /* Shift remaining jobs down */
    for (int i = found_idx; i < table->num_jobs - 1; i++) {
        table->jobs[i] = table->jobs[i + 1];
    }
    table->num_jobs--;
    memset(&table->jobs[table->num_jobs], 0, sizeof(Job));
    table->jobs[table->num_jobs].out_pipe_fd = -1;

    /* Re-evaluate current and previous jobs */
    if (table->current_job_id == id) {
        table->current_job_id = table->previous_job_id;
        table->previous_job_id = 0;
        /* Pick any other active job for previous_job_id */
        for (int i = table->num_jobs - 1; i >= 0; i--) {
            if (table->jobs[i].id != table->current_job_id) {
                table->previous_job_id = table->jobs[i].id;
                break;
            }
        }
    } else if (table->previous_job_id == id) {
        table->previous_job_id = 0;
        for (int i = table->num_jobs - 1; i >= 0; i--) {
            if (table->jobs[i].id != table->current_job_id) {
                table->previous_job_id = table->jobs[i].id;
                break;
            }
        }
    }

    return 0;
}

void jobs_kill_all(JobTable *table) {
    if (!table) return;

    for (int i = 0; i < table->num_jobs; i++) {
        Job *j = &table->jobs[i];
        if (j->pgid > 0) {
            kill(-j->pgid, SIGTERM);
            kill(j->pid, SIGTERM);
            kill(-j->pgid, SIGCONT); /* Resume stopped jobs so they terminate */
        }
        if (j->out_pipe_fd >= 0) {
            close(j->out_pipe_fd);
            j->out_pipe_fd = -1;
        }
    }

    for (int i = 0; i < table->num_jobs; i++) {
        Job *j = &table->jobs[i];
        if (j->pid > 0) {
            int status = 0;
            pid_t p = waitpid(j->pid, &status, WNOHANG);
            if (p == 0) {
                kill(-j->pgid, SIGKILL);
                kill(j->pid, SIGKILL);
                waitpid(j->pid, &status, 0);
            }
        }
    }

    table->num_jobs = 0;
    table->current_job_id = 0;
    table->previous_job_id = 0;
}

void jobs_print(const JobTable *table, char *out_buf, size_t max_len) {
    if (!table || !out_buf || max_len == 0) return;
    out_buf[0] = '\0';

    for (int i = 0; i < table->num_jobs; i++) {
        const Job *j = &table->jobs[i];

        char sign = ' ';
        if (j->id == table->current_job_id) {
            sign = '+';
        } else if (j->id == table->previous_job_id) {
            sign = '-';
        }

        const char *state_str = "Running";
        if (j->state == JOB_STATE_STOPPED) {
            state_str = "Stopped";
        } else if (j->state == JOB_STATE_DONE) {
            state_str = "Done";
        }

        char line[256];
        if (j->state == JOB_STATE_RUNNING) {
            snprintf(line, sizeof(line), "[%d]%c  %-24s%s &\n", j->id, sign, state_str, j->command);
        } else {
            snprintf(line, sizeof(line), "[%d]%c  %-24s%s\n", j->id, sign, state_str, j->command);
        }

        size_t cur_len = strlen(out_buf);
        if (cur_len + strlen(line) < max_len) {
            strncat(out_buf, line, max_len - cur_len - 1);
        }
    }
}

void jobs_check_status(JobTable *table, char *notification_buf, size_t max_len) {
    if (!table || !notification_buf || max_len == 0) return;

    for (int i = 0; i < table->num_jobs; i++) {
        Job *j = &table->jobs[i];
        int status = 0;
        pid_t w = waitpid(j->pid, &status, WNOHANG | WUNTRACED | WCONTINUED);

        if (w > 0) {
            char sign = (j->id == table->current_job_id) ? '+' :
                        (j->id == table->previous_job_id) ? '-' : ' ';

            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                /* Job terminated */
                char notif[256];
                snprintf(notif, sizeof(notif), "[%d]%c  Done                    %s\n",
                         j->id, sign, j->command);

                size_t cur_len = strlen(notification_buf);
                if (cur_len + strlen(notif) < max_len) {
                    strncat(notification_buf, notif, max_len - cur_len - 1);
                }

                if (j->out_pipe_fd >= 0) {
                    close(j->out_pipe_fd);
                    j->out_pipe_fd = -1;
                }

                int rem_id = j->id;
                jobs_remove(table, rem_id);
                i--; /* Re-examine current index since elements shifted */
            } else if (WIFSTOPPED(status)) {
                if (j->state != JOB_STATE_STOPPED) {
                    j->state = JOB_STATE_STOPPED;
                    char notif[256];
                    snprintf(notif, sizeof(notif), "[%d]%c  Stopped                 %s\n",
                             j->id, sign, j->command);
                    size_t cur_len = strlen(notification_buf);
                    if (cur_len + strlen(notif) < max_len) {
                        strncat(notification_buf, notif, max_len - cur_len - 1);
                    }
                }
            } else if (WIFCONTINUED(status)) {
                j->state = JOB_STATE_RUNNING;
            }
        }
    }
}

/**
 * @brief Helper to parse job ID from argument like "%1", "1", "%+", "%-".
 */
static int parse_job_arg(JobTable *table, const char *arg) {
    if (!arg) {
        return table->current_job_id;
    }

    if (arg[0] == '%') {
        arg++;
    }

    if (*arg == '\0' || strcmp(arg, "+") == 0) {
        return table->current_job_id;
    } else if (strcmp(arg, "-") == 0) {
        return table->previous_job_id;
    }

    return atoi(arg);
}

int jobs_builtin_jobs(JobTable *table, char **argv, char *out_buf, size_t max_len) {
    (void)argv;
    jobs_print(table, out_buf, max_len);
    return 0;
}

int jobs_builtin_bg(JobTable *table, char **argv, char *out_buf, size_t max_len) {
    if (!table || !out_buf || max_len == 0) return 1;
    out_buf[0] = '\0';

    const char *arg = (argv && argv[1]) ? argv[1] : NULL;
    int job_id = parse_job_arg(table, arg);

    Job *j = jobs_find_by_id(table, job_id);
    if (!j) {
        snprintf(out_buf, max_len, "terminator: bg: %s: no such job\n", arg ? arg : "current");
        return 1;
    }

    /* Send SIGCONT to the process group */
    if (j->pgid > 0) {
        kill(-j->pgid, SIGCONT);
        kill(j->pid, SIGCONT);
    }
    j->state = JOB_STATE_RUNNING;

    /* Update current job */
    table->previous_job_id = table->current_job_id;
    table->current_job_id = j->id;

    snprintf(out_buf, max_len, "[%d]+ %s &\n", j->id, j->command);
    return 0;
}

int jobs_builtin_fg(JobTable *table, char **argv, char *out_buf, size_t max_len, Job *res_job) {
    if (!table || !out_buf || max_len == 0 || !res_job) return 1;
    out_buf[0] = '\0';

    const char *arg = (argv && argv[1]) ? argv[1] : NULL;
    int job_id = parse_job_arg(table, arg);

    Job *j = jobs_find_by_id(table, job_id);
    if (!j) {
        snprintf(out_buf, max_len, "terminator: fg: %s: no such job\n", arg ? arg : "current");
        return 1;
    }

    /* Copy job details to caller */
    *res_job = *j;

    /* Remove from background table */
    jobs_remove(table, job_id);

    return 0;
}
