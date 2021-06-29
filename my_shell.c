//
//  main.c
//  shell
//
//  Created by Denis Furyaev on 30.11.2019.
//  Copyright © 2019 Denis Furyaev. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <time.h>

/*
 
 Grammar:
 
     shell_cmd          := conditional_cmd { [";" | "&"] shell_cmd } { "&" }
     conditional_cmd    := cmd { ["&&" | "||" ] conditional_cmd }
     cmd                := { io_redirect } pipeline | pipeline { io_redirect } | "(" shell_cmd ")"
     io_redirect        := { i_redirect } o_redirect | { o_redirect } i_redirect
     i_redirect         := "<" LITERAL
     o_redirect         := ">" LITERAL | ">>" LITERAL
     pipeline           := simple_cmd { "|" pipeline }
     simple_cmd         := LITERAL { LITERAL }
 
*/
// Lexer -----------------------------------------------------------------------------------------------------------------//

typedef enum {
    PIPE, SEMICOLON, AMP, DOUBLE_AMP, DOUBLE_PIPE, SMALLER, GRATER, DOUBLE_GRATER, LT_BRACE, RT_BRACE,
    LITERAL, EPSILON
} Symbol;

const char * symbol_str[] = {"|", ";", "&", "&&", "||", "<", ">", ">>", "(", ")", "LITERAL", "EPSILON"};

Symbol symbol;
char ** tokens = NULL;
char * matched_token = NULL;
int next_token = 0;
int debug_level = 1;

Symbol str_to_symbol(const char * str) {
    if (!strcmp(str, "|"))   return PIPE;
    if (!strcmp(str, ";"))   return SEMICOLON;
    if (!strcmp(str, "&"))   return AMP;
    if (!strcmp(str, "&&"))  return DOUBLE_AMP;
    if (!strcmp(str, "||"))  return DOUBLE_PIPE;
    if (!strcmp(str, "<"))   return SMALLER;
    if (!strcmp(str, ">"))   return GRATER;
    if (!strcmp(str, ">>"))  return DOUBLE_GRATER;
    if (!strcmp(str, "("))   return LT_BRACE;
    if (!strcmp(str, ")"))   return RT_BRACE;
    return LITERAL;
}

void next_symbol() {
    if (tokens[next_token] != NULL) {
        symbol = str_to_symbol(tokens[next_token]);
        if (debug_level > 1)
            printf("        token : %s %s\n", tokens[next_token], symbol_str[symbol]);
        next_token++;
    }
    else
        symbol = EPSILON;
}

#define LINE_BUFSIZE 1024
char * read_line(void) {
    int bufsize = LINE_BUFSIZE, position = 0;
    char * buffer = malloc(bufsize);
    int c;
    
    while (1) {
        c = getchar();
        
        if (c == EOF || c == '\n') {
            buffer[position] = '\0';
            return buffer;
        }
        else
            buffer[position] = c;
        
        position++;
        if (position >= bufsize) {
            bufsize += LINE_BUFSIZE;
            buffer = realloc(buffer, bufsize);
        }
    }
}

#define TOKENS_BUFSIZE 64
#define TOKENS_DELIMETERS " \t\r\n"
char ** tokenize(char * line) {
    int position = 0;
    char ** tokens = malloc(TOKENS_BUFSIZE);
    char * token;
    
    token = strtok(line, TOKENS_DELIMETERS);
    while (token != NULL) {
        tokens[position] = token;
        position++;
        token = strtok(NULL, TOKENS_DELIMETERS);
    }
    tokens[position] = NULL;
    return tokens;
}

// Interpreter -----------------------------------------------------------------------------------------------------------//

typedef enum {DONT_SKIP_NEXT, SKIP_NEXST_ON_FAILURE, SKIP_NEXST_ON_SUCCSESS} RunCondition;

typedef struct {
    char **         cmd_list[64];
    int             cmd_list_size;
    char *          io_redirect[2];
    int             append;
    RunCondition    run_condition;
    int             run_in_background;
} PipelineData;

void init_pipeline(PipelineData * pipeline_data) {
    pipeline_data->cmd_list_size = 0;
    pipeline_data->io_redirect[0] = NULL;
    pipeline_data->io_redirect[1] = NULL;
    pipeline_data->append = 0;
    pipeline_data->run_condition = DONT_SKIP_NEXT;
    pipeline_data->run_in_background = 0;
}

// Parser ----------------------------------------------------------------------------------------------------------------//

int match(Symbol s) {
    if (debug_level > 1)
        printf("    matching  : [%s] [%s]\n", symbol_str[s], symbol_str[symbol]);
    if (s == symbol) {
        matched_token = tokens[next_token - 1];
        next_symbol();
        return 1;
    }
    matched_token = NULL;
    return 0;
}

int expect(Symbol s) {
    if (match(s))
        return 1;
    printf("syntax error : expected %s, found %s\n", symbol_str[s], symbol_str[symbol]);
    exit(-1);
    return 0;
}

//------------------------------------------------------------------------------------//

// simple_cmd := LITERAL { LITERAL_LIST }
char ** arg_list;
int simple_cmd() {
    if (debug_level > 1)
        printf("parsing <simple_cmd>\n");
    if (match(LITERAL)) {
        arg_list = malloc(64 * sizeof(char *));
        arg_list[0] = matched_token;
        int c = 1;
        while (match(LITERAL)) {
            arg_list[c] = matched_token;
            c++;
        }
        arg_list[c] = NULL;
        if (debug_level) {
            printf("simple cmd args: ");
            for (int c = 0; c < next_token; c++) {
                printf("%s ", arg_list[c]);
            }
            printf("\n");
        }
        return 1;
    }
    return 0;
}

// pipeline := simple_cmd { "|" pipeline }
PipelineData pipeline_data;
int pipeline() {
    if (debug_level > 1)
        printf("parsing <pipeline>\n");
    if (simple_cmd()) {
        pipeline_data.cmd_list[pipeline_data.cmd_list_size++] = arg_list;
        arg_list = NULL;
        if (match(PIPE)) {
            pipeline();
        }
        return 1;
    }
    return 0;
}

// o_redirect := ">" LITERAL | ">>" LITERAL
int o_redirect() {
    if (debug_level > 1)
        printf("parsing <o_redirect>\n");
    if (match(GRATER)) {
        if (expect(LITERAL)) {
            pipeline_data.io_redirect[1] = matched_token;
            return 1;
        }
        return 0;
    }
    if (match(DOUBLE_GRATER)) {
        if (expect(LITERAL)) {
            pipeline_data.append = 1;
            pipeline_data.io_redirect[1] = matched_token;
            return 1;
        }
        return 0;
    }
    return 0;
}

// i_redirect := "<" LITERAL
int i_redirect() {
    if (debug_level > 1)
        printf("parsing <i_redirect>\n");
    if (match(SMALLER)) {
        if (expect(LITERAL)) {
            pipeline_data.io_redirect[0] = matched_token;
            return 1;
        }
    }
    return 0;
}

// io_redirect := { i_redirect } o_redirect | { o_redirect } i_redirect
int io_redirect() {
    if (debug_level > 1)
        printf("parsing <io_redirect>\n");
    if (i_redirect()) {
        if (o_redirect())
            return 1;
        return 1;
    }
    if (o_redirect()) {
        if (i_redirect())
            return 1;
        return 1;
    }
    return 0;
}

// cmd := { io_redirect } pipeline | pipeline { io_redirect } | "(" shell_cmd ")"
int shell_cmd(void);
int run_exernal_shell(char * str);
int cmd() {
    if (debug_level > 1)
        printf("parsing <cmd>\n");
    if (match(LT_BRACE)) {
        char * str = malloc(LINE_BUFSIZE);
        while (!match(EPSILON)) {
            if (match(RT_BRACE))
                break;
            if (strlen(str) != 0)
                strcat(str, " ");
            strcat(str, tokens[next_token - 1]);
            next_symbol();
        }
        return run_exernal_shell(str);
    }
    if (io_redirect()) {
        if (pipeline()) {
            return 1;
        }
        else
            return 0;
    }
    if (pipeline()) {
        if (io_redirect()) {
            return 1;
        }
        return 1;
    }
    return 0;
}

PipelineData pipeline_data_list[64];
int pipeline_data_list_size = 0;

void clear_all() {
    pipeline_data_list_size = 0;
    free(tokens);
}

// conditional_cmd := cmd { ["&&" | "||" ] conditional_cmd }
int conditional_cmd() {
    if (debug_level > 1)
        printf("parsing <conditional_cmd>\n");
    init_pipeline(&pipeline_data);
    if (cmd()) {
        pipeline_data_list[pipeline_data_list_size++] = pipeline_data;
        if (match(DOUBLE_AMP)) {
            pipeline_data_list[pipeline_data_list_size - 1].run_condition = SKIP_NEXST_ON_FAILURE;
            return conditional_cmd();
        }
        else
            if (match(DOUBLE_PIPE)) {
                pipeline_data_list[pipeline_data_list_size - 1].run_condition = SKIP_NEXST_ON_SUCCSESS;
                return conditional_cmd();
            }
        return 1;
    }
    return 0;
}


int execute_pipeline(PipelineData * pipeline, int fd) {
    int fd_prev[2] = {fd, -1};
    if (pipeline->io_redirect[0])
        fd_prev[0] = open(pipeline->io_redirect[0], O_RDONLY);
    if (pipeline->run_in_background) {
        fd_prev[0] = open("/dev/null", O_RDONLY);
    }
    
    for (int i = 0; i < pipeline->cmd_list_size; i++) {
        int is_last = (i == (pipeline->cmd_list_size - 1));
        char ** cmd = pipeline->cmd_list[i];
        
        int fd_next[2] = {-1, -1};
        if (!is_last)
            pipe(fd_next);
        else {
            if (pipeline->io_redirect[1]) {
                int flags = O_WRONLY | O_CREAT;
                if (pipeline->append)
                    flags |= O_APPEND;
                else
                    flags |= O_TRUNC;
                fd_next[1] = open(pipeline->io_redirect[1], flags, 0666);
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork error");
            exit(-1);
        }
        if (pid) {
            // parent
            
            close(fd_prev[0]);
            close(fd_prev[1]);
            
            fd_prev[0] = fd_next[0];
            fd_prev[1] = fd_next[1];
            
            int status = 0;
            if (is_last && !pipeline->run_in_background) {
                waitpid(pid, &status, 0);
                return (status == 0) ? 1 : 0;
            }
            else {
                waitpid(pid, &status, WNOHANG);
            }
            
        } else {
            // child
            
            if (pipeline->run_in_background)
                signal(SIGINT,SIG_IGN);
            
            if (debug_level) {
                printf("child started: %d\n", pid);
                printf("flag = %d\n", pipeline->run_in_background);
            }
            
            close(fd_next[0]);
            close(fd_prev[1]);
            
            if (fd_next[1] != -1)
                dup2(fd_next[1], 1);
            if (fd_prev[0] != -1)
                dup2(fd_prev[0], 0);
            
            execvp(cmd[0], cmd);
            perror("execvp failed:");
            exit(-1);
        }
    }
    return 0;
}

char * prog_name = NULL;
int run_exernal_shell(char * str) {
    PipelineData pipeline;
    init_pipeline(&pipeline);
    pipeline.cmd_list[0] = malloc(2 * sizeof(char *));
    pipeline.cmd_list[0][0] = prog_name;
    pipeline.cmd_list[0][1] = NULL;
    pipeline.cmd_list_size = 1;
    
    int fd[2];
    pipe(fd);
    write(fd[1], str, strlen(str));
    close(fd[1]);
    
    return execute_pipeline(&pipeline, fd[0]);
}

int execute_pipelines() {
    for (int i = 0; i < pipeline_data_list_size; i++) {
        PipelineData * pipeline = &pipeline_data_list[i];
        if (debug_level > 0) {
            printf("Pipeline #%d\n", i);
            printf("  Redirect input: %s ; out: %s \n", pipeline->io_redirect[0], pipeline->io_redirect[1]);
        }

        if (debug_level > 0) {
            for (int c = 0; c < pipeline->cmd_list_size; c++) {
                printf("  Command #%d: ", c);
                int j = 0;
                while (pipeline->cmd_list[c][j]) {
                    printf(" \"%s\"", pipeline->cmd_list[c][j]);
                    j++;
                }
                printf("\n");
            }
        }
        
        int result = execute_pipeline(pipeline, -1);
        
//        if (!(pipeline->run_in_background))
//            waitpid(-1, 0, WNOHANG);

        
        if (pipeline->run_condition == SKIP_NEXST_ON_FAILURE && !result)
            i++; // break; if like in the original shell
        if (pipeline->run_condition == SKIP_NEXST_ON_SUCCSESS && result)
            i++; // break; if like in the original shell
    }
    return 0;
}

// shell_cmd := conditional_cmd { [";" | "&"] shell_cmd } { "&" }
int shell_cmd() {
    if (debug_level > 1)
        printf("parsing <shell_cmd>\n");
    if (conditional_cmd()) {
        int result = 1;
        if (match(SEMICOLON))
            result = shell_cmd();
        else if (match(AMP)) {
            pipeline_data_list[pipeline_data_list_size - 1].run_in_background = 1;
            result = shell_cmd();
        }
        if (result) {
            if (match(AMP)) {
                pipeline_data_list[pipeline_data_list_size - 1].run_in_background = 1;
                return 1;
            }
            return expect(EPSILON);
        }
        return 1; // changed from 0 to 1 !!!!!!!!
    }
    return 0;
}

// Main_loop -------------------------------------------------------------------------------------------------------------//

int main(int argc, char * argv[]) {
    printf("Shell started:\n");
    prog_name = argv[0];
    char * str;
    while (1) {
        printf("> ");
        str = read_line();
        if (debug_level > 0)
            printf("Input string: \"%s\"\n", str);
        if (str[0] == '\0')
            break;
        tokens = tokenize(str);
        next_token = 0;
        next_symbol();
        int result = shell_cmd();
        
        if (result)
            execute_pipelines();
        clear_all();
        if (debug_level > 0)
            printf("Parsed with result: %d\n", result);
        free(str);

        for (int i = 0; i < 10; i++)
            waitpid(-1, NULL, WNOHANG);
    }

    return 0;
}

// -----------------------------------------------------------------------------------------------------------------------//
