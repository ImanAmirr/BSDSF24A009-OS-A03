#include "shell.h"
#include <fcntl.h>

static void child_fail(const char* operation) {
    perror(operation);
    _exit(EXIT_FAILURE);
}

int execute_pipeline(const pipeline_t* pipeline, char* history[],
                     int history_count, int history_start, int history_total,
                     const background_job_t jobs[], int job_count,
                     pid_t spawned_pids[], int* spawned_count) {
    int pipes[MAX_PIPELINE - 1][2];
    pid_t child_pids[MAX_PIPELINE];
    int pipe_count = pipeline->command_count - 1;

    for (int i = 0; i < pipe_count; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return -1;
        }
    }

    int child_count = 0;
    for (int i = 0; i < pipeline->command_count; i++) {
        pid_t child_pid = fork();
        if (child_pid < 0) {
            perror("fork");
            break;
        }
        if (child_pid == 0) {
            if (i > 0 && dup2(pipes[i - 1][0], STDIN_FILENO) < 0) {
                child_fail("dup2 pipe input");
            }
            if (i < pipe_count && dup2(pipes[i][1], STDOUT_FILENO) < 0) {
                child_fail("dup2 pipe output");
            }

            for (int j = 0; j < pipe_count; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            const shell_command_t* command = &pipeline->commands[i];
            if (command->input_file != NULL) {
                int input_fd = open(command->input_file, O_RDONLY);
                if (input_fd < 0) child_fail(command->input_file);
                if (dup2(input_fd, STDIN_FILENO) < 0) child_fail("dup2 input");
                close(input_fd);
            }
            if (command->output_file != NULL) {
                int output_fd = open(command->output_file,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (output_fd < 0) child_fail(command->output_file);
                if (dup2(output_fd, STDOUT_FILENO) < 0) child_fail("dup2 output");
                close(output_fd);
            }

            int builtin_result = handle_builtin(command->arglist, history,
                                                history_count, history_start,
                                                history_total, jobs, job_count);
            if (builtin_result != BUILTIN_NOT_FOUND) {
                fflush(stdout);
                _exit(builtin_result);
            }

            execvp(command->arglist[0], command->arglist);
            perror(command->arglist[0]);
            _exit(127);
        }

        child_pids[child_count++] = child_pid;
    }

    for (int i = 0; i < pipe_count; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    *spawned_count = child_count;
    for (int i = 0; i < child_count; i++) {
        spawned_pids[i] = child_pids[i];
    }

    if (child_count != pipeline->command_count) {
        for (int i = 0; i < child_count; i++) {
            while (waitpid(child_pids[i], NULL, 0) < 0 && errno == EINTR) {}
        }
        *spawned_count = 0;
        return -1;
    }
    if (pipeline->background) {
        return 0;
    }

    int final_status = 0;
    for (int i = 0; i < child_count; i++) {
        int status;
        pid_t waited_pid;
        do {
            waited_pid = waitpid(child_pids[i], &status, 0);
        } while (waited_pid < 0 && errno == EINTR);
        if (waited_pid < 0) {
            perror("waitpid");
            final_status = 1;
        } else if (i == child_count - 1) {
            if (WIFEXITED(status)) {
                final_status = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                final_status = 128 + WTERMSIG(status);
            } else {
                final_status = 1;
            }
        }
    }
    return final_status;
}
