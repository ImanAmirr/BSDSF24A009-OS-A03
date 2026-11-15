#include "shell.h"
#include <readline/readline.h>
#include <dirent.h>

static char* copy_string(const char* source) {
    size_t length = strlen(source) + 1;
    char* copy = (char*)malloc(length);
    if (copy != NULL) {
        memcpy(copy, source, length);
    }
    return copy;
}

static const char* builtin_commands[] = {
    "cd", "exit", "help", "history", "jobs", NULL
};

static char* command_generator(const char* text, int state) {
    static int builtin_index;
    static char* path_list;
    static char* path_cursor;
    static DIR* directory;
    static char* directory_name;
    static size_t prefix_length;

    if (state == 0) {
        if (directory != NULL) {
            closedir(directory);
            directory = NULL;
        }
        free(path_list);
        const char* path = getenv("PATH");
        path_list = copy_string(path != NULL ? path : "");
        path_cursor = path_list;
        directory_name = NULL;
        builtin_index = 0;
        prefix_length = strlen(text);
    }

    while (builtin_commands[builtin_index] != NULL) {
        const char* candidate = builtin_commands[builtin_index++];
        if (strncmp(candidate, text, prefix_length) == 0) {
            return copy_string(candidate);
        }
    }

    if (path_list == NULL) {
        return NULL;
    }

    while (1) {
        if (directory == NULL) {
            directory_name = strtok_r(path_cursor, ":", &path_cursor);
            if (directory_name == NULL) {
                return NULL;
            }
            directory = opendir(directory_name);
            if (directory == NULL) {
                continue;
            }
        }

        struct dirent* entry = readdir(directory);
        if (entry == NULL) {
            closedir(directory);
            directory = NULL;
            continue;
        }

        if (strncmp(entry->d_name, text, prefix_length) == 0) {
            size_t path_length = strlen(directory_name) + strlen(entry->d_name) + 2;
            char* full_path = (char*)malloc(path_length);
            if (full_path == NULL) {
                continue;
            }
            snprintf(full_path, path_length, "%s/%s", directory_name, entry->d_name);
            int is_executable = access(full_path, X_OK) == 0;
            free(full_path);
            if (is_executable) {
                return copy_string(entry->d_name);
            }
        }
    }
}

char** shell_completion(const char* text, int start, int end) {
    (void)end;
    if (start == 0 && strchr(text, '/') == NULL) {
        return rl_completion_matches(text, command_generator);
    }
    return NULL;
}

static int is_operator(char character) {
    return character == '<' || character == '>' || character == '|';
}

void free_pipeline(pipeline_t* pipeline) {
    for (int i = 0; i < pipeline->command_count; i++) {
        shell_command_t* command = &pipeline->commands[i];
        for (int j = 0; command->arglist[j] != NULL; j++) {
            free(command->arglist[j]);
        }
        free(command->input_file);
        free(command->output_file);
    }
    memset(pipeline, 0, sizeof(*pipeline));
}

int parse_line(char* cmdline, pipeline_t* pipeline) {
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->command_count = 1;
    char* first = cmdline;
    while (*first == ' ' || *first == '\t') first++;
    if (*first == '\0') return 0;

    int current_command = 0;
    char pending_redirection = '\0';
    size_t line_length = strlen(cmdline);
    char* cursor = cmdline;

    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (*cursor == '\0') break;

        if (is_operator(*cursor)) {
            char operation = *cursor++;
            shell_command_t* command = &pipeline->commands[current_command];
            if (operation == '|') {
                if (pending_redirection != '\0' || command->arglist[0] == NULL ||
                    pipeline->command_count == MAX_PIPELINE) {
                    fprintf(stderr, "syntax error near '|'.\n");
                    free_pipeline(pipeline);
                    return 0;
                }
                current_command = pipeline->command_count++;
            } else {
                if (pending_redirection != '\0') {
                    fprintf(stderr, "syntax error: missing redirection target.\n");
                    free_pipeline(pipeline);
                    return 0;
                }
                pending_redirection = operation;
            }
            continue;
        }

        char* word = (char*)malloc(line_length + 1);
        if (word == NULL) {
            perror("parse");
            free_pipeline(pipeline);
            return 0;
        }
        size_t word_length = 0;
        char quote = '\0';
        int word_started = 0;
        while (*cursor != '\0') {
            char character = *cursor;
            if (quote == '\0' && (character == ' ' || character == '\t' ||
                                   is_operator(character))) {
                break;
            }
            cursor++;
            if (quote != '\0' && character == quote) {
                quote = '\0';
                word_started = 1;
            } else if (quote == '\0' && (character == '\'' || character == '"')) {
                quote = character;
                word_started = 1;
            } else if (character == '\\' && quote != '\'' && *cursor != '\0') {
                word[word_length++] = *cursor++;
                word_started = 1;
            } else {
                word[word_length++] = character;
                word_started = 1;
            }
        }
        if (quote != '\0') {
            fprintf(stderr, "syntax error: unmatched quote.\n");
            free(word);
            free_pipeline(pipeline);
            return 0;
        }
        if (!word_started) {
            free(word);
            continue;
        }
        word[word_length] = '\0';

        shell_command_t* command = &pipeline->commands[current_command];
        if (pending_redirection == '<') {
            if (command->input_file != NULL) {
                fprintf(stderr, "syntax error: duplicate input redirection.\n");
                free(word);
                free_pipeline(pipeline);
                return 0;
            }
            command->input_file = word;
            pending_redirection = '\0';
        } else if (pending_redirection == '>') {
            if (command->output_file != NULL) {
                fprintf(stderr, "syntax error: duplicate output redirection.\n");
                free(word);
                free_pipeline(pipeline);
                return 0;
            }
            command->output_file = word;
            pending_redirection = '\0';
        } else {
            int arg_count = 0;
            while (command->arglist[arg_count] != NULL) arg_count++;
            if (arg_count == MAXARGS) {
                fprintf(stderr, "parse: too many arguments.\n");
                free(word);
                free_pipeline(pipeline);
                return 0;
            }
            command->arglist[arg_count] = word;
        }
    }

    if (pending_redirection != '\0') {
        fprintf(stderr, "syntax error: missing redirection target.\n");
        free_pipeline(pipeline);
        return 0;
    }
    for (int i = 0; i < pipeline->command_count; i++) {
        if (pipeline->commands[i].arglist[0] == NULL) {
            fprintf(stderr, "syntax error: missing command.\n");
            free_pipeline(pipeline);
            return 0;
        }
    }
    return 1;
}

int is_builtin_command(const char* command) {
    return strcmp(command, "cd") == 0 || strcmp(command, "exit") == 0 ||
           strcmp(command, "help") == 0 || strcmp(command, "history") == 0 ||
           strcmp(command, "jobs") == 0;
}

int expand_history(char** cmdline, char* history[], int history_count,
                   int history_start, int history_total) {
    if ((*cmdline)[0] != '!') {
        return 0;
    }

    char* end;
    char* digits = *cmdline + 1;
    if (*digits == '\0') {
        fprintf(stderr, "history: invalid event: %s\n", *cmdline);
        return -1;
    }
    for (char* digit = digits; *digit != '\0'; digit++) {
        if (*digit < '0' || *digit > '9') {
            fprintf(stderr, "history: invalid event: %s\n", *cmdline);
            return -1;
        }
    }

    errno = 0;
    long requested = strtol(digits, &end, 10);
    if (errno == ERANGE || end == *cmdline + 1 || *end != '\0' || requested <= 0) {
        fprintf(stderr, "history: invalid event: %s\n", *cmdline);
        return -1;
    }

    int oldest = history_total - history_count + 1;
    if (requested < oldest || requested > history_total) {
        fprintf(stderr, "history: event %ld not found\n", requested);
        return -1;
    }

    int slot = (history_start + (int)requested - oldest) % HISTORY_SIZE;
    char* expanded = copy_string(history[slot]);
    if (expanded == NULL) {
        perror("history");
        return -1;
    }

    free(*cmdline);
    *cmdline = expanded;
    return 1;
}

void add_to_history(char* history[], int* history_count, int* history_start,
                    int* history_total, const char* cmdline) {
    char* copy = copy_string(cmdline);
    if (copy == NULL) {
        perror("history");
        return;
    }

    int slot;
    if (*history_count < HISTORY_SIZE) {
        slot = (*history_start + *history_count) % HISTORY_SIZE;
        (*history_count)++;
    } else {
        slot = *history_start;
        free(history[slot]);
        *history_start = (*history_start + 1) % HISTORY_SIZE;
    }

    history[slot] = copy;
    (*history_total)++;
}

int handle_builtin(char* const arglist[], char* history[], int history_count,
                   int history_start, int history_total) {
    if (strcmp(arglist[0], "exit") == 0) {
        return 1;
    }

    if (strcmp(arglist[0], "cd") == 0) {
        if (arglist[1] == NULL) {
            fprintf(stderr, "cd: missing directory\n");
        } else if (chdir(arglist[1]) != 0) {
            perror("cd");
        }
        return 1;
    }

    if (strcmp(arglist[0], "help") == 0) {
        printf("Built-in commands: cd <directory>, exit, help, history, jobs\n");
        return 1;
    }

    if (strcmp(arglist[0], "history") == 0) {
        int first_number = history_total - history_count + 1;
        for (int i = 0; i < history_count; i++) {
            int slot = (history_start + i) % HISTORY_SIZE;
            printf("%d %s\n", first_number + i, history[slot]);
        }
        return 1;
    }

    if (strcmp(arglist[0], "jobs") == 0) {
        printf("Job control not yet implemented.\n");
        return 1;
    }

    return 0;
}
