#include "shell.h"
#include <readline/readline.h>
#include <readline/history.h>
#include <fcntl.h>

static int run_builtin(const shell_command_t* command, char* history[],
                       int history_count, int history_start, int history_total) {
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
                                history_start, history_total);
    fflush(stdout);
    if (dup2(saved_stdin, STDIN_FILENO) < 0 ||
        dup2(saved_stdout, STDOUT_FILENO) < 0) {
        perror("restore standard streams");
    }
    close(saved_stdin);
    close(saved_stdout);
    return result;
}

int main() {
    char* cmdline;
    char* history[HISTORY_SIZE] = { NULL };
    int history_count = 0;
    int history_start = 0;
    int history_total = 0;
    int should_exit = 0;

    using_history();
    rl_readline_name = "myshell";
    rl_attempted_completion_function = shell_completion;
    rl_bind_key('\t', rl_complete);

    while (!should_exit && (cmdline = readline(PROMPT)) != NULL) {
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

        pipeline_t pipeline;
        if (parse_line(cmdline, &pipeline)) {
            shell_command_t* first = &pipeline.commands[0];
            if (pipeline.command_count == 1 && is_builtin_command(first->arglist[0])) {
                int builtin_result = run_builtin(first, history, history_count,
                                                 history_start, history_total);
                if (builtin_result && strcmp(first->arglist[0], "exit") == 0) {
                    should_exit = 1;
                }
            } else {
                execute_pipeline(&pipeline, history, history_count,
                                 history_start, history_total);
            }
            free_pipeline(&pipeline);
        }
        free(cmdline);
    }

    for (int i = 0; i < HISTORY_SIZE; i++) {
        free(history[i]);
    }

    printf("\nShell exited.\n");
    return 0;
}
