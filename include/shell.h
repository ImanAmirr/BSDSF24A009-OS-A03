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
#define MAX_PIPELINE 10
#define PROMPT "FCIT> "

typedef struct {
    char* arglist[MAXARGS + 1];
    char* input_file;
    char* output_file;
} shell_command_t;

typedef struct {
    shell_command_t commands[MAX_PIPELINE];
    int command_count;
} pipeline_t;

// Function prototypes
char** shell_completion(const char* text, int start, int end);
int parse_line(char* cmdline, pipeline_t* pipeline);
void free_pipeline(pipeline_t* pipeline);
int is_builtin_command(const char* command);
int expand_history(char** cmdline, char* history[], int history_count,
                   int history_start, int history_total);
void add_to_history(char* history[], int* history_count, int* history_start,
                    int* history_total, const char* cmdline);
int handle_builtin(char* const arglist[], char* history[], int history_count,
                   int history_start, int history_total);
int execute_pipeline(const pipeline_t* pipeline, char* history[],
                     int history_count, int history_start, int history_total);

#endif // SHELL_H
