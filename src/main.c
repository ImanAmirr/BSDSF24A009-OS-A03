#include "shell.h"

int main() {
    char* cmdline;
    char** arglist;
    char* history[HISTORY_SIZE] = { NULL };
    int history_count = 0;
    int history_start = 0;
    int history_total = 0;
    int should_exit = 0;

    while (!should_exit && (cmdline = read_cmd(PROMPT, stdin)) != NULL) {
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

        if ((arglist = tokenize(cmdline)) != NULL) {
            int builtin_result = handle_builtin(arglist, history, history_count,
                                                history_start, history_total);
            if (builtin_result && strcmp(arglist[0], "exit") == 0) {
                should_exit = 1;
            } else if (!builtin_result) {
                execute(arglist);
            }

            // Free the memory allocated by tokenize()
            for (int i = 0; arglist[i] != NULL; i++) {
                free(arglist[i]);
            }
            free(arglist);
        }
        free(cmdline);
    }

    for (int i = 0; i < HISTORY_SIZE; i++) {
        free(history[i]);
    }

    printf("\nShell exited.\n");
    return 0;
}
