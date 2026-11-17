#include "shell.h"
#include <readline/readline.h>
#include <dirent.h>
#include <stdint.h>

static char* copy_string(const char* source) {
    size_t length = strlen(source) + 1;
    char* copy = (char*)malloc(length);
    if (copy != NULL) {
        memcpy(copy, source, length);
    }
    return copy;
}

static int is_variable_name_start(char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') || character == '_';
}

static int is_variable_name_character(char character) {
    return is_variable_name_start(character) ||
           (character >= '0' && character <= '9');
}

static const char* find_variable_value(const shell_variable_t* variables,
                                      const char* name, size_t name_length) {
    for (const shell_variable_t* variable = variables;
         variable != NULL; variable = variable->next) {
        if (strlen(variable->name) == name_length &&
            memcmp(variable->name, name, name_length) == 0) {
            return variable->value;
        }
    }
    return NULL;
}

static char* expand_text(const char* text, const shell_variable_t* variables) {
    size_t output_length = 0;
    for (size_t i = 0; text[i] != '\0';) {
        if (text[i] == '$' && is_variable_name_start(text[i + 1])) {
            size_t name_start = i + 1;
            size_t name_end = name_start + 1;
            while (is_variable_name_character(text[name_end])) name_end++;
            const char* value = find_variable_value(variables, text + name_start,
                                                    name_end - name_start);
            size_t value_length = value != NULL ? strlen(value) : 0;
            if (value_length > SIZE_MAX - output_length - 1) return NULL;
            output_length += value_length;
            i = name_end;
        } else {
            if (output_length == SIZE_MAX - 1) return NULL;
            output_length++;
            i++;
        }
    }

    char* expanded = (char*)malloc(output_length + 1);
    if (expanded == NULL) return NULL;

    size_t output_index = 0;
    for (size_t i = 0; text[i] != '\0';) {
        if (text[i] == '$' && is_variable_name_start(text[i + 1])) {
            size_t name_start = i + 1;
            size_t name_end = name_start + 1;
            while (is_variable_name_character(text[name_end])) name_end++;
            const char* value = find_variable_value(variables, text + name_start,
                                                    name_end - name_start);
            if (value != NULL) {
                size_t value_length = strlen(value);
                memcpy(expanded + output_index, value, value_length);
                output_index += value_length;
            }
            i = name_end;
        } else {
            expanded[output_index++] = text[i++];
        }
    }
    expanded[output_index] = '\0';
    return expanded;
}

static const char* builtin_commands[] = {
    "cd", "exit", "help", "history", "jobs", "set", NULL
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
    free(pipeline->command_line);
    memset(pipeline, 0, sizeof(*pipeline));
}

void free_command_list(command_list_t* commands) {
    for (int i = 0; i < commands->pipeline_count; i++) {
        free_pipeline(&commands->pipelines[i]);
    }
    memset(commands, 0, sizeof(*commands));
}

static int parse_pipeline(char* cmdline, pipeline_t* pipeline) {
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

static int add_pipeline_segment(char* segment, int background,
                                command_list_t* commands) {
    char* start = segment;
    while (*start == ' ' || *start == '\t') start++;
    char* end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (end == start) {
        if (background) {
            fprintf(stderr, "syntax error: '&' requires a command.\n");
            return 0;
        }
        return 1;
    }
    *end = '\0';

    if (commands->pipeline_count == MAX_CHAIN) {
        fprintf(stderr, "parse: too many chained commands.\n");
        return 0;
    }

    pipeline_t* pipeline = &commands->pipelines[commands->pipeline_count];
    if (!parse_pipeline(start, pipeline)) {
        return 0;
    }
    pipeline->background = background;

    size_t source_length = strlen(start);
    pipeline->command_line = (char*)malloc(source_length + (background ? 3 : 1));
    if (pipeline->command_line == NULL) {
        perror("parse");
        free_pipeline(pipeline);
        return 0;
    }
    memcpy(pipeline->command_line, start, source_length);
    if (background) {
        memcpy(pipeline->command_line + source_length, " &", 3);
    } else {
        pipeline->command_line[source_length] = '\0';
    }
    commands->pipeline_count++;
    return 1;
}

int parse_line(char* cmdline, command_list_t* commands) {
    memset(commands, 0, sizeof(*commands));
    char* segment = cmdline;
    char quote = '\0';

    for (char* cursor = cmdline; ; cursor++) {
        char character = *cursor;
        if (character == '\0') {
            if (quote != '\0') {
                fprintf(stderr, "syntax error: unmatched quote.\n");
                free_command_list(commands);
                return 0;
            }
            if (!add_pipeline_segment(segment, 0, commands)) {
                free_command_list(commands);
                return 0;
            }
            break;
        }

        if (quote != '\'' && character == '\\' && cursor[1] != '\0') {
            cursor++;
            continue;
        }
        if (quote == '\0' && (character == '\'' || character == '"')) {
            quote = character;
            continue;
        }
        if (quote != '\0' && character == quote) {
            quote = '\0';
            continue;
        }
        if (quote == '\0' && (character == ';' || character == '&')) {
            *cursor = '\0';
            if (!add_pipeline_segment(segment, character == '&', commands)) {
                free_command_list(commands);
                return 0;
            }
            segment = cursor + 1;
        }
    }

    return commands->pipeline_count > 0;
}

int is_builtin_command(const char* command) {
    return strcmp(command, "cd") == 0 || strcmp(command, "exit") == 0 ||
           strcmp(command, "help") == 0 || strcmp(command, "history") == 0 ||
           strcmp(command, "jobs") == 0 || strcmp(command, "set") == 0;
}

int is_variable_assignment(const pipeline_t* pipeline) {
    if (pipeline->background || pipeline->command_count != 1) return 0;
    const shell_command_t* command = &pipeline->commands[0];
    if (command->input_file != NULL || command->output_file != NULL ||
        command->arglist[0] == NULL || command->arglist[1] != NULL) {
        return 0;
    }

    const char* equals = strchr(command->arglist[0], '=');
    if (equals == NULL || equals == command->arglist[0] ||
        !is_variable_name_start(command->arglist[0][0])) {
        return 0;
    }
    for (const char* character = command->arglist[0];
         character < equals; character++) {
        if (!is_variable_name_character(*character)) return 0;
    }
    return 1;
}

int assign_variable(shell_variable_t** variables, const char* assignment) {
    const char* equals = strchr(assignment, '=');
    if (equals == NULL || equals == assignment ||
        !is_variable_name_start(assignment[0])) {
        fprintf(stderr, "assignment: invalid variable name.\n");
        return 1;
    }
    for (const char* character = assignment; character < equals; character++) {
        if (!is_variable_name_character(*character)) {
            fprintf(stderr, "assignment: invalid variable name.\n");
            return 1;
        }
    }

    size_t name_length = (size_t)(equals - assignment);
    char* name = (char*)malloc(name_length + 1);
    char* value = copy_string(equals + 1);
    if (name == NULL || value == NULL) {
        free(name);
        free(value);
        fprintf(stderr, "assignment: unable to allocate variable.\n");
        return 1;
    }
    memcpy(name, assignment, name_length);
    name[name_length] = '\0';

    for (shell_variable_t* variable = *variables;
         variable != NULL; variable = variable->next) {
        if (strcmp(variable->name, name) == 0) {
            free(variable->value);
            variable->value = value;
            free(name);
            return 0;
        }
    }

    shell_variable_t* variable = (shell_variable_t*)malloc(sizeof(*variable));
    if (variable == NULL) {
        free(name);
        free(value);
        fprintf(stderr, "assignment: unable to allocate variable.\n");
        return 1;
    }
    variable->name = name;
    variable->value = value;
    variable->next = *variables;
    *variables = variable;
    return 0;
}

int expand_pipeline_variables(pipeline_t* pipeline,
                              const shell_variable_t* variables) {
    for (int i = 0; i < pipeline->command_count; i++) {
        shell_command_t* command = &pipeline->commands[i];
        int first_argument = i == 0 && is_variable_assignment(pipeline) ? 0 : 1;
        for (int j = first_argument; command->arglist[j] != NULL; j++) {
            char* expanded = expand_text(command->arglist[j], variables);
            if (expanded == NULL) {
                fprintf(stderr, "variable expansion: unable to allocate value.\n");
                return 0;
            }
            free(command->arglist[j]);
            command->arglist[j] = expanded;
        }
        if (command->input_file != NULL) {
            char* expanded = expand_text(command->input_file, variables);
            if (expanded == NULL) {
                fprintf(stderr, "variable expansion: unable to allocate value.\n");
                return 0;
            }
            free(command->input_file);
            command->input_file = expanded;
        }
        if (command->output_file != NULL) {
            char* expanded = expand_text(command->output_file, variables);
            if (expanded == NULL) {
                fprintf(stderr, "variable expansion: unable to allocate value.\n");
                return 0;
            }
            free(command->output_file);
            command->output_file = expanded;
        }
    }
    return 1;
}

void free_variables(shell_variable_t** variables) {
    shell_variable_t* variable = *variables;
    while (variable != NULL) {
        shell_variable_t* next = variable->next;
        free(variable->name);
        free(variable->value);
        free(variable);
        variable = next;
    }
    *variables = NULL;
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
                   int history_start, int history_total,
                   const background_job_t jobs[], int job_count,
                   const shell_variable_t* variables) {
    if (strcmp(arglist[0], "exit") == 0) {
        return 0;
    }

    if (strcmp(arglist[0], "cd") == 0) {
        if (arglist[1] == NULL) {
            fprintf(stderr, "cd: missing directory\n");
            return 1;
        } else if (chdir(arglist[1]) != 0) {
            perror("cd");
            return 1;
        }
        return 0;
    }

    if (strcmp(arglist[0], "help") == 0) {
        printf("Built-in commands: cd <directory>, exit, help, history, jobs, set\n");
        return 0;
    }

    if (strcmp(arglist[0], "history") == 0) {
        int first_number = history_total - history_count + 1;
        for (int i = 0; i < history_count; i++) {
            int slot = (history_start + i) % HISTORY_SIZE;
            printf("%d %s\n", first_number + i, history[slot]);
        }
        return 0;
    }

    if (strcmp(arglist[0], "jobs") == 0) {
        for (int i = 0; i < job_count; i++) {
            printf("[%d]", i + 1);
            for (int j = 0; j < jobs[i].process_count; j++) {
                printf(" %ld", (long)jobs[i].pids[j]);
            }
            printf(" %s\n", jobs[i].command_line);
        }
        return 0;
    }

    if (strcmp(arglist[0], "set") == 0) {
        for (const shell_variable_t* variable = variables;
             variable != NULL; variable = variable->next) {
            printf("%s=%s\n", variable->name, variable->value);
        }
        return 0;
    }

    return BUILTIN_NOT_FOUND;
}
