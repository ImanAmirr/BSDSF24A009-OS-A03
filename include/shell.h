#ifndef SHELL_H
#define SHELL_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_LEN 512
#define MAXARGS 10
#define ARGLEN 30
#define HISTORY_SIZE 20
#define PROMPT "FCIT> "

// Function prototypes
char* read_cmd(char* prompt, FILE* fp);
char** tokenize(char* cmdline);
int expand_history(char** cmdline, char* history[], int history_count,
                   int history_start, int history_total);
void add_to_history(char* history[], int* history_count, int* history_start,
                    int* history_total, const char* cmdline);
int handle_builtin(char** arglist, char* history[], int history_count,
                   int history_start, int history_total);
int execute(char** arglist);

#endif // SHELL_H
