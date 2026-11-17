#include "shell.h"
#include <readline/readline.h>
#include <readline/history.h>
#include <fcntl.h>

static int run_builtin(const shell_command_t* command, char* history[],
                       int history_count, int history_start, int history_total,
                       const background_job_t jobs[], int job_count,
                       const shell_variable_t* variables) {
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
                                history_start, history_total, jobs, job_count,
                                variables);
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

static char* duplicate_text(const char* text) {
    size_t length = strlen(text) + 1;
    char* copy = (char*)malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

static char* trim_line(char* line) {
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
    size_t length = strlen(line);
    while (length > 0 && (line[length - 1] == ' ' || line[length - 1] == '\t' ||
                          line[length - 1] == '\r' || line[length - 1] == '\n')) {
        line[--length] = '\0';
    }
    return line;
}

static int run_command_line(char* line, char* history[], int history_count,
                            int history_start, int history_total,
                            background_job_t jobs[], int* job_count,
                            int* should_exit, shell_variable_t** variables) {
    command_list_t commands;
    if (!parse_line(line, &commands)) return 1;

    int final_status = 0;
    for (int i = 0; i < commands.pipeline_count && !*should_exit; i++) {
        reap_background_jobs(jobs, job_count);
        pipeline_t* pipeline = &commands.pipelines[i];

        /* Expand arguments before deciding whether this is an assignment,
         * built-in, or external command. */
        if (!expand_pipeline_variables(pipeline, *variables)) {
            final_status = 1;
            continue;
        }

        if (is_variable_assignment(pipeline)) {
            final_status = assign_variable(
                variables, pipeline->commands[0].arglist[0]);
            continue;
        }

        shell_command_t* first = &pipeline->commands[0];
        if (!pipeline->background && pipeline->command_count == 1 &&
            is_builtin_command(first->arglist[0])) {
            final_status = run_builtin(first, history, history_count,
                                       history_start, history_total,
                                       jobs, *job_count, *variables);
            if (strcmp(first->arglist[0], "exit") == 0) {
                *should_exit = 1;
            }
        } else {
            pid_t spawned_pids[MAX_PIPELINE];
            int spawned_count = 0;
            final_status = execute_pipeline(pipeline, history, history_count,
                                           history_start, history_total,
                                           jobs, *job_count,
                                           *variables,
                                           spawned_pids, &spawned_count);
            if (final_status < 0) final_status = 1;
            if (pipeline->background) {
                register_background_job(jobs, job_count, spawned_pids,
                                        spawned_count, pipeline->command_line);
            }
        }
    }
    free_command_list(&commands);
    return final_status;
}

static int is_if_command(const char* line) {
    while (*line == ' ' || *line == '\t') line++;
    return line[0] == 'i' && line[1] == 'f' &&
           (line[2] == ' ' || line[2] == '\t' || line[2] == '\0');
}

static void free_lines(char* lines[], int count) {
    for (int i = 0; i < count; i++) free(lines[i]);
}

static int is_control_space(char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n' || character == ';';
}

static int find_control_word(const char* text, const char* word,
                             size_t* word_start, size_t* word_end) {
    char quote = '\0';
    for (size_t i = 0; text[i] != '\0'; i++) {
        char character = text[i];
        if (character == '\\' && quote != '\'' && text[i + 1] != '\0') {
            i++;
            continue;
        }
        if (quote == '\0' && (character == '\'' || character == '"')) {
            quote = character;
            continue;
        }
        if (quote != '\0') {
            if (character == quote) quote = '\0';
            continue;
        }

        size_t word_length = strlen(word);
        if (strncmp(text + i, word, word_length) == 0 &&
            (i == 0 || is_control_space(text[i - 1])) &&
            (text[i + word_length] == '\0' ||
             is_control_space(text[i + word_length]))) {
            *word_start = i;
            *word_end = i + word_length;
            return 1;
        }
    }
    return 0;
}

static int store_if_body_line(char* lines[], int* count, int capacity,
                              const char* text, size_t length) {
    while (length > 0 && (*text == ' ' || *text == '\t' || *text == '\r' ||
                          *text == '\n')) {
        text++;
        length--;
    }
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                          text[length - 1] == '\r' || text[length - 1] == '\n')) {
        length--;
    }
    if (length == 0) return 1;
    if (*count == capacity) {
        fprintf(stderr, "if: block exceeds %d command lines.\n", capacity);
        return 0;
    }

    char* copy = (char*)malloc(length + 1);
    if (copy == NULL) {
        perror("if");
        return 0;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    lines[(*count)++] = copy;
    return 1;
}

static int consume_if_body(char* text, int* in_else, int* complete,
                           char* then_lines[], int* then_count,
                           char* else_lines[], int* else_count, int capacity) {
    size_t offset = 0;
    while (text[offset] != '\0') {
        size_t else_start = 0, else_end = 0, fi_start = 0, fi_end = 0;
        int has_else = find_control_word(text + offset, "else",
                                         &else_start, &else_end);
        int has_fi = find_control_word(text + offset, "fi", &fi_start, &fi_end);
        if (!has_else && !has_fi) {
            char** selected_lines = *in_else ? else_lines : then_lines;
            int* selected_count = *in_else ? else_count : then_count;
            return store_if_body_line(selected_lines, selected_count, capacity,
                                      text + offset, strlen(text + offset));
        }
        int marker_is_else = has_else && (!has_fi || else_start < fi_start);
        size_t marker_start = marker_is_else ? else_start : fi_start;
        size_t marker_end = marker_is_else ? else_end : fi_end;

        char** selected_lines = *in_else ? else_lines : then_lines;
        int* selected_count = *in_else ? else_count : then_count;
        if (!store_if_body_line(selected_lines, selected_count, capacity,
                                text + offset, marker_start)) {
            return 0;
        }
        if (marker_is_else) {
            if (*in_else) {
                fprintf(stderr, "if: unexpected 'else'.\n");
                return 0;
            }
            *in_else = 1;
            offset += marker_end;
        } else {
            const char* tail = text + offset + marker_end;
            while (*tail == ' ' || *tail == '\t' || *tail == '\r' ||
                   *tail == '\n') tail++;
            if (*tail != '\0') {
                fprintf(stderr, "if: unexpected text after 'fi'.\n");
                return 0;
            }
            *complete = 1;
            return 1;
        }
    }

    char** selected_lines = *in_else ? else_lines : then_lines;
    int* selected_count = *in_else ? else_count : then_count;
    return store_if_body_line(selected_lines, selected_count, capacity,
                              text + offset, strlen(text + offset));
}

static int run_if_block(const char* if_line, char* history[], int* history_count,
                        int* history_start, int* history_total,
                        background_job_t jobs[], int* job_count,
                        int* should_exit, shell_variable_t** variables) {
    const char* condition = if_line;
    while (*condition == ' ' || *condition == '\t') condition++;
    enum { MAX_IF_LINES = 100 };
    char* then_lines[MAX_IF_LINES] = { 0 };
    char* else_lines[MAX_IF_LINES] = { 0 };
    int then_count = 0;
    int else_count = 0;
    int saw_then = 0;
    int in_else = 0;
    int complete = 0;
    int valid = 1;

    condition += 2;
    while (*condition == ' ' || *condition == '\t') condition++;
    size_t then_start = 0, then_end = 0;
    int has_inline_then = find_control_word(condition, "then", &then_start, &then_end);
    size_t condition_length = has_inline_then ? then_start : strlen(condition);
    while (condition_length > 0 &&
           (condition[condition_length - 1] == ' ' ||
            condition[condition_length - 1] == '\t' ||
            condition[condition_length - 1] == '\r' ||
            condition[condition_length - 1] == '\n')) {
        condition_length--;
    }
    if (condition_length == 0) {
        fprintf(stderr, "if: expected a condition command after 'if'.\n");
        return 1;
    }

    char* condition_line = (char*)malloc(condition_length + 1);
    if (condition_line == NULL) {
        perror("if");
        return 1;
    }
    memcpy(condition_line, condition, condition_length);
    condition_line[condition_length] = '\0';

    if (has_inline_then) {
        saw_then = 1;
        valid = consume_if_body((char*)condition + then_end, &in_else,
                                &complete, then_lines, &then_count,
                                else_lines, &else_count, MAX_IF_LINES);
    }

    while (!complete && valid) {
        char* line = readline("if> ");
        if (line == NULL) {
            fprintf(stderr, "if: unexpected end of input before 'fi'.\n");
            valid = 0;
            break;
        }
        if (line[0] != '\0') {
            add_history(line);
            add_to_history(history, history_count, history_start,
                           history_total, line);
        }

        char* trimmed = trim_line(line);
        if (!saw_then && strcmp(trimmed, "then") == 0) {
            saw_then = 1;
        } else if (saw_then && strcmp(trimmed, "else") == 0) {
            if (in_else) {
                fprintf(stderr, "if: unexpected 'else'.\n");
                valid = 0;
            } else {
                in_else = 1;
            }
        } else if (saw_then && strcmp(trimmed, "fi") == 0) {
            complete = 1;
        } else if (!saw_then) {
            size_t start = 0, end = 0;
            if (!find_control_word(trimmed, "then", &start, &end)) {
                if (*trimmed != '\0') {
                    fprintf(stderr, "if: expected 'then' before block commands.\n");
                    valid = 0;
                }
            } else {
                for (size_t i = 0; i < start; i++) {
                    if (trimmed[i] != ' ' && trimmed[i] != '\t' && trimmed[i] != '\r') {
                        fprintf(stderr, "if: expected 'then' before block commands.\n");
                        valid = 0;
                        break;
                    }
                }
                if (valid) {
                    saw_then = 1;
                    valid = consume_if_body(trimmed + end, &in_else, &complete,
                                            then_lines, &then_count,
                                            else_lines, &else_count, MAX_IF_LINES);
                }
            }
        } else {
            valid = consume_if_body(line, &in_else, &complete,
                                    then_lines, &then_count,
                                    else_lines, &else_count, MAX_IF_LINES);
        }
        free(line);
    }

    int status = 1;
    if (valid && complete) {
        status = run_command_line(condition_line, history, *history_count,
                                  *history_start, *history_total,
                                  jobs, job_count, should_exit, variables);
        char** selected_lines = status == 0 ? then_lines : else_lines;
        int selected_count = status == 0 ? then_count : else_count;
        for (int i = 0; i < selected_count && !*should_exit; i++) {
            char* command = duplicate_text(selected_lines[i]);
            if (command == NULL) {
                perror("if");
                status = 1;
                break;
            }
            status = run_command_line(command, history, *history_count,
                                      *history_start, *history_total,
                                      jobs, job_count, should_exit, variables);
            free(command);
        }
    }

    free(condition_line);
    free_lines(then_lines, then_count);
    free_lines(else_lines, else_count);
    return status;
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
    shell_variable_t* variables = NULL;

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

        if (is_if_command(cmdline)) {
            run_if_block(cmdline, history, &history_count, &history_start,
                         &history_total, jobs, &job_count, &should_exit,
                         &variables);
        } else {
            run_command_line(cmdline, history, history_count, history_start,
                             history_total, jobs, &job_count, &should_exit,
                             &variables);
        }
        free(cmdline);
    }

    for (int i = 0; i < HISTORY_SIZE; i++) {
        free(history[i]);
    }
    for (int i = 0; i < job_count; i++) {
        free(jobs[i].command_line);
    }
    free_variables(&variables);

    printf("\nShell exited.\n");
    return 0;
}
