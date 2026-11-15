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
#define MAX_CHAIN 20
#define MAX_JOBS 64
#define PROMPT "FCIT> "
#define BUILTIN_NOT_FOUND -1

typedef struct {
    char* arglist[MAXARGS + 1];
    char* input_file;
    char* output_file;
} shell_command_t;

typedef struct {
    shell_command_t commands[MAX_PIPELINE];
    int command_count;
    int background;
    char* command_line;
} pipeline_t;

typedef struct {
    pipeline_t pipelines[MAX_CHAIN];
    int pipeline_count;
} command_list_t;

typedef struct {
    pid_t pids[MAX_PIPELINE];
    int process_count;
    char* command_line;
} background_job_t;

// Function prototypes
char** shell_completion(const char* text, int start, int end);
int parse_line(char* cmdline, command_list_t* commands);
void free_pipeline(pipeline_t* pipeline);
void free_command_list(command_list_t* commands);
int is_builtin_command(const char* command);
int expand_history(char** cmdline, char* history[], int history_count,
                   int history_start, int history_total);
void add_to_history(char* history[], int* history_count, int* history_start,
                    int* history_total, const char* cmdline);
int handle_builtin(char* const arglist[], char* history[], int history_count,
                   int history_start, int history_total,
                   const background_job_t jobs[], int job_count);
int execute_pipeline(const pipeline_t* pipeline, char* history[],
                     int history_count, int history_start, int history_total,
                     const background_job_t jobs[], int job_count,
                     pid_t spawned_pids[], int* spawned_count);

#endif // SHELL_H
