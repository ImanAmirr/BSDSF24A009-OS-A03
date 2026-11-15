#include "shell.h"

static char* copy_string(const char* source) {
    size_t length = strlen(source) + 1;
    char* copy = (char*)malloc(length);
    if (copy != NULL) {
        memcpy(copy, source, length);
    }
    return copy;
}

char* read_cmd(char* prompt, FILE* fp) {
    printf("%s", prompt);
    char* cmdline = (char*) malloc(sizeof(char) * MAX_LEN);
    int c, pos = 0;

    while ((c = getc(fp)) != EOF) {
        if (c == '\n') break;
        cmdline[pos++] = c;
    }

    if (c == EOF && pos == 0) {
        free(cmdline);
        return NULL; // Handle Ctrl+D
    }

    cmdline[pos] = '\0';
    return cmdline;
}

char** tokenize(char* cmdline) {
    // Edge case: empty command line
    if (cmdline == NULL || cmdline[0] == '\0' || cmdline[0] == '\n') {
        return NULL;
    }

    char** arglist = (char**)malloc(sizeof(char*) * (MAXARGS + 1));
    for (int i = 0; i < MAXARGS + 1; i++) {
        arglist[i] = (char*)malloc(sizeof(char) * ARGLEN);
        bzero(arglist[i], ARGLEN);
    }

    char* cp = cmdline;
    char* start;
    int len;
    int argnum = 0;

    while (*cp != '\0' && argnum < MAXARGS) {
        while (*cp == ' ' || *cp == '\t') cp++; // Skip leading whitespace

        if (*cp == '\0') break; // Line was only whitespace

        start = cp;
        len = 1;
        while (*++cp != '\0' && !(*cp == ' ' || *cp == '\t')) {
            len++;
        }
        strncpy(arglist[argnum], start, len);
        arglist[argnum][len] = '\0';
        argnum++;
    }

    if (argnum == 0) { // No arguments were parsed
        for(int i = 0; i < MAXARGS + 1; i++) free(arglist[i]);
        free(arglist);
        return NULL;
    }

    arglist[argnum] = NULL;
    return arglist;
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

int handle_builtin(char** arglist, char* history[], int history_count,
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
