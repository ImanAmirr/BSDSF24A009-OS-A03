#include "shell.h"
#include <readline/readline.h>
#include <readline/history.h>
#include <fcntl.h>

static int run_builtin(const shell_command_t* command, char* history[],
                       int history_count, int history_start, int history_total,
                       const background_job_t jobs[], int job_count) {
    int input_fd = -1;
    int output_fd = -1;

    if (command->input_file != NULL) {
        input_fd = open(command->input_file, O_RDONLY);
        if (input_fd < 0) {
            perror(command->input_file);
            return 1;
        }
    }
    if (command->output_file != NULL) {
        output_fd = open(command->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (output_fd < 0) {
            perror(command->output_file);
            if (input_fd >= 0) close(input_fd);
            return 1;
        }
    }

    int saved_stdin = dup(STDIN_FILENO);
    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdin < 0 || saved_stdout < 0) {
        perror("redirection");
        if (saved_stdin >= 0) close(saved_stdin);
        if (saved_stdout >= 0) close(saved_stdout);
        if (input_fd >= 0) close(input_fd);
        if (output_fd >= 0) close(output_fd);
        return 1;
    }
    if ((input_fd >= 0 && dup2(input_fd, STDIN_FILENO) < 0) ||
        (output_fd >= 0 && dup2(output_fd, STDOUT_FILENO) < 0)) {
        perror("redirection");
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        if (input_fd >= 0) close(input_fd);
        if (output_fd >= 0) close(output_fd);
        return 1;
    }

    if (input_fd >= 0) close(input_fd);
    if (output_fd >= 0) close(output_fd);

    int result = handle_builtin(command->arglist, history, history_count,
                                history_start, history_total, jobs, job_count);
    fflush(stdout);
    if (dup2(saved_stdin, STDIN_FILENO) < 0 ||
        dup2(saved_stdout, STDOUT_FILENO) < 0) {
        perror("restore standard streams");
    }
    close(saved_stdin);
    close(saved_stdout);
    return result;
}

static void reap_background_jobs(background_job_t jobs[], int* job_count) {
    while (1) {
        int status;
        pid_t finished_pid = waitpid(-1, &status, WNOHANG);
        if (finished_pid == 0) return;
        if (finished_pid < 0) {
            if (errno == EINTR) continue;
            if (errno != ECHILD) perror("waitpid");
            return;
        }

        for (int i = 0; i < *job_count; i++) {
            background_job_t* job = &jobs[i];
            for (int j = 0; j < job->process_count; j++) {
                if (job->pids[j] == finished_pid) {
                    for (int k = j + 1; k < job->process_count; k++) {
                        job->pids[k - 1] = job->pids[k];
                    }
                    job->process_count--;
                    break;
                }
            }
        }

        for (int i = 0; i < *job_count; i++) {
            if (jobs[i].process_count == 0) {
                free(jobs[i].command_line);
                for (int j = i + 1; j < *job_count; j++) {
                    jobs[j - 1] = jobs[j];
                }
                (*job_count)--;
                memset(&jobs[*job_count], 0, sizeof(jobs[*job_count]));
                i--;
            }
        }
    }
}

static void register_background_job(background_job_t jobs[], int* job_count,
                                    const pid_t pids[], int process_count,
                                    const char* command_line) {
    if (process_count == 0) return;
    if (*job_count == MAX_JOBS) {
        fprintf(stderr, "jobs: job list is full; waiting for this command.\n");
        for (int i = 0; i < process_count; i++) {
            while (waitpid(pids[i], NULL, 0) < 0 && errno == EINTR) {}
        }
        return;
    }

    size_t length = strlen(command_line) + 1;
    char* copy = (char*)malloc(length);
    if (copy == NULL) {
        perror("jobs");
        for (int i = 0; i < process_count; i++) {
            while (waitpid(pids[i], NULL, 0) < 0 && errno == EINTR) {}
        }
        return;
    }
    memcpy(copy, command_line, length);

    background_job_t* job = &jobs[*job_count];
    job->process_count = process_count;
    job->command_line = copy;
    for (int i = 0; i < process_count; i++) {
        job->pids[i] = pids[i];
    }
    (*job_count)++;
}

int main() {
    char* cmdline;
    char* history[HISTORY_SIZE] = { NULL };
    int history_count = 0;
    int history_start = 0;
    int history_total = 0;
    int should_exit = 0;
    background_job_t jobs[MAX_JOBS] = { 0 };
    int job_count = 0;

    using_history();
    rl_readline_name = "myshell";
    rl_attempted_completion_function = shell_completion;
    rl_bind_key('\t', rl_complete);

    while (!should_exit) {
        reap_background_jobs(jobs, &job_count);
        cmdline = readline(PROMPT);
        if (cmdline == NULL) break;

        if (cmdline[0] != '\0') {
            add_history(cmdline);
        }

        int expansion_result = expand_history(&cmdline, history, history_count,
                                              history_start, history_total);
        if (expansion_result < 0) {
            free(cmdline);
            continue;
        }

        if (cmdline[0] != '\0') {
            add_to_history(history, &history_count, &history_start,
                           &history_total, cmdline);
        }

        command_list_t commands;
        if (parse_line(cmdline, &commands)) {
            for (int i = 0; i < commands.pipeline_count && !should_exit; i++) {
                reap_background_jobs(jobs, &job_count);
                pipeline_t* pipeline = &commands.pipelines[i];
                shell_command_t* first = &pipeline->commands[0];
                if (!pipeline->background && pipeline->command_count == 1 &&
                    is_builtin_command(first->arglist[0])) {
                    int builtin_result = run_builtin(first, history, history_count,
                                                     history_start, history_total,
                                                     jobs, job_count);
                    if (builtin_result && strcmp(first->arglist[0], "exit") == 0) {
                        should_exit = 1;
                    }
                } else {
                    pid_t spawned_pids[MAX_PIPELINE];
                    int spawned_count = 0;
                    execute_pipeline(pipeline, history, history_count,
                                     history_start, history_total,
                                     jobs, job_count,
                                     spawned_pids, &spawned_count);
                    if (pipeline->background) {
                        register_background_job(jobs, &job_count, spawned_pids,
                                                spawned_count,
                                                pipeline->command_line);
                    }
                }
            }
            free_command_list(&commands);
        }
        free(cmdline);
    }

    for (int i = 0; i < HISTORY_SIZE; i++) {
        free(history[i]);
    }
    for (int i = 0; i < job_count; i++) {
        free(jobs[i].command_line);
    }

    printf("\nShell exited.\n");
    return 0;
}
