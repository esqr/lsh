#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <ctype.h>

#define OK          0
#define NO_INPUT    1
#define TOO_LONG    2
#define GOT_EOT     3
#define INTERRUPTED 4

#define WRITE_END   1
#define READ_END    0

#define SYMNONE     0
#define SYMPIPE     1
#define SYMREDIRIN  2
#define SYMREDIROUT 3
#define SYMREDIRERR 4
#define SYMASYNC    5

int running = 1;
int sigflag = 0;

static int get_line (char *buff, size_t sz) {
    char c;
    int cc = 0;

    while (sigflag == 0 && (c = fgetc(stdin)) != '\n') {
        if (cc == sz - 1) {
            return TOO_LONG;
        }

        if (c != EOF) {
            buff[cc++] = c;
        } else if (cc == 0 && sigflag == 0) {
            return GOT_EOT;
        }

    }

    if (sigflag != 0) {
        return INTERRUPTED;
    }

    if (cc == 0) {
        return NO_INPUT;
    }

    buff[cc] = '\0';

    return OK;
}

void build_prompt(char **prompt_ptr, char *cwd) {
    free(*prompt_ptr);
    //asprintf(prompt_ptr, "%s%% ", cwd);
    asprintf(prompt_ptr, "lsh$ ");
}

char *strstrip(char *s) {
    size_t size;
    char *end;

    size = strlen(s);

    if (!size)
        return s;

    end = s + size - 1;
    while (end >= s && isspace(*end))
        end--;
    *(end + 1) = '\0';

    while (*s && isspace(*s))
        s++;

    return s;
}

void parse_command(char *line, char **argv) {
    while (*line != '\0') {
        while (*line == ' ' || *line == '\t' || *line == '\n') {
            *line++ = '\0';
        }

        *argv++ = line;

        while (*line != '\0' && *line != ' ' &&
               *line != '\t' && *line != '\n') {
            line++;
        }
    }

    *argv = '\0';
}

void parse_line(char *line, char **commands, int *symbols) {
    int i = 0;

    while (*line != '\0') {
        while (*line == '&' || *line == '|' || *line == '<' || *line == '>') {
            switch (*line) {
                case '&':
                    symbols[i] = SYMASYNC;
                    break;
                case '|':
                    symbols[i] = SYMPIPE;
                    break;
                case '<':
                    symbols[i] = SYMREDIRIN;
                    break;
                case '>':
                    if (*(line - 1) == '2') {
                        symbols[i] = SYMREDIRERR;
                        *(line - 1) = '\0';
                    } else {
                        symbols[i] = SYMREDIROUT;
                    }
                    break;
                default:
                    break;
            }

            i++;

            *line++ = '\0';
        }

        *commands++ = line;

        while (*line != '\0' && *line != '&' && *line != '|' && *line != '<'
               && *line != '>') {
            line++;
        }
    }
    symbols[i] = SYMNONE;

    *commands = NULL;
}

void builtin_exit() {
    printf("exit\n");
    running = 0;
}

struct {
    char *command;
    void (*function)();
} builtin_commands[] = {
        {"exit", builtin_exit}
};


int execute(char **argv, int *pipe_rd, int *pipe_wr, int *pipe_wr_out, int *pipe_wr_err) {
    pid_t pid = fork();
    if (pid == 0) {
        if (pipe_wr != NULL) {
            if (pipe_wr[READ_END] != -1) {
                close(pipe_wr[READ_END]);
            }

            //fprintf(stderr, "%s wr: %d\n", *argv, pipe_wr[WRITE_END]);
            if (dup2(pipe_wr[WRITE_END], STDOUT_FILENO) == -1) {
                fprintf(stderr, "dup2 write pipe failed; errno: %d\n", errno);
                exit(1);
            }

            close(pipe_wr[WRITE_END]);

        }
        
        if (pipe_wr_out != NULL) {
            if (pipe_wr_out[READ_END] != -1) {
                close(pipe_wr_out[READ_END]);
            }

            //fprintf(stderr, "%s wr out: %d\n", *argv, pipe_wr_out[WRITE_END]);
            if (dup2(pipe_wr_out[WRITE_END], STDOUT_FILENO) == -1) {
                fprintf(stderr, "dup2 write pipe failed; errno: %d\n", errno);
                exit(1);
            }

            if (pipe_wr_out[WRITE_END] != STDOUT_FILENO && pipe_wr_out[WRITE_END] != STDERR_FILENO) {
                close(pipe_wr_out[WRITE_END]);
            }

        }

        if (pipe_wr_err != NULL) {
            if (pipe_wr_err[READ_END] != -1) {
                close(pipe_wr_err[READ_END]);
            }

            //fprintf(stderr, "%s wr err: %d\n", *argv, pipe_wr_err[WRITE_END]);
            if (dup2(pipe_wr_err[WRITE_END], STDERR_FILENO) == -1) {
                fprintf(stderr, "dup2 write pipe failed; errno: %d\n", errno);
                exit(1);
            }

            if (pipe_wr_err[WRITE_END] != STDOUT_FILENO && pipe_wr_err[WRITE_END] != STDERR_FILENO) {
                close(pipe_wr_err[WRITE_END]);
            }

        }

        if (pipe_rd != NULL) {
            if (pipe_rd[WRITE_END] != -1) {
                close(pipe_rd[WRITE_END]);
            }

            //fprintf(stderr, "%s rd: %d\n", *argv, pipe_rd[READ_END]);
            if (dup2(pipe_rd[READ_END], STDIN_FILENO) == -1) {
                fprintf(stderr, "dup2 read pipe failed; errno: %d\n", errno);
                exit(1);
            }

            close(pipe_rd[READ_END]);
        }

        if (execvp(*argv, argv) < 0) {
            if (errno == ENOENT) {
                fprintf(stderr, "%s: command not found\n", *argv);
            } else {
                fprintf(stderr, "execvp failed: %d\n", errno);
            }
        }

        exit(1);
    } else {
        return pid;
    }
}

void process_input(char *input) {
    for (int i = 0; i < (int) (sizeof (builtin_commands) /
                               sizeof (builtin_commands[0])); i++) {
        if (strcmp(input, builtin_commands[i].command) == 0) {
            builtin_commands[i].function();
            return;
        }
    }

    char *commands[64];
    int symbols[64];
    int wait_queue[64];
    int wait_count = 0;

    parse_line(input, commands, symbols);

    int *last_pipefd = NULL;

    for (int i = 0; commands[i] != NULL; i++) {
        char *argv[64];
        commands[i] = strstrip(commands[i]);
        parse_command(commands[i], argv);

        int *pipe_rd = last_pipefd;
        int *pipe_wr = NULL;
        int *pipe_wr_out = NULL;
        int *pipe_wr_err = NULL;
        int should_wait = 1;
        int next_sym = 1;
        int fd;

        while (next_sym) {
            switch (symbols[i]) {
                case SYMPIPE:
                    should_wait = 0;
                    pipe_wr = malloc(2 * sizeof(int));
                    pipe(pipe_wr);
                    next_sym = 0;
                    break;
                case SYMREDIRIN:
                    pipe_rd = malloc(2 * sizeof(int));
                    ++i;
                    commands[i] = strstrip(commands[i]);
                    pipe_rd[READ_END] = open(commands[i], O_RDONLY);
                    pipe_rd[WRITE_END] = -1;
                    break;
                case SYMREDIROUT:
                    ++i;
                    commands[i] = strstrip(commands[i]);
                    pipe_wr_out = malloc(2 * sizeof(int));
                    pipe_wr_out[READ_END] = -1;

                    if (symbols[i] == SYMASYNC && *commands[i] != '\0' && (fd = strtol(commands[i], NULL, 10)) > 0) {
                        pipe_wr_out[WRITE_END] = fd;
                        memmove(symbols + i, symbols + i + 1, (64 - i - 1) * sizeof (*symbols));
                    } else {
                        pipe_wr_out[WRITE_END] = open(commands[i], O_WRONLY | O_CREAT, 0644);
                    }
                    break;
                case SYMREDIRERR:
                    ++i;
                    commands[i] = strstrip(commands[i]);
                    pipe_wr_err = malloc(2 * sizeof(int));
                    pipe_wr_err[READ_END] = -1;

                    if (symbols[i] == SYMASYNC && *commands[i] != '\0' && (fd = strtol(commands[i], NULL, 10)) > 0) {
                        pipe_wr_err[WRITE_END] = fd;
                        memmove(symbols + i, symbols + i + 1, (64 - i - 1) * sizeof (*symbols));
                    } else {
                        pipe_wr_err[WRITE_END] = open(commands[i], O_WRONLY | O_CREAT, 0644);
                    }
                    break;
                case SYMASYNC:
                    should_wait = 0;
                    next_sym = 0;
                    break;
                default:
                    next_sym = 0;
                    break;
            }
        }


        pid_t pid = execute(argv, pipe_rd, pipe_wr, pipe_wr_out, pipe_wr_err);

        if (pipe_rd != NULL) {
            if (pipe_rd[WRITE_END] != -1) {
                close(pipe_rd[WRITE_END]);
            }

            if (pipe_rd[READ_END] != -1) {
                close(pipe_rd[READ_END]);
            }
        }

        if (should_wait) {
            wait_queue[wait_count++] = pid;
        }


        free(pipe_rd);
        free(pipe_wr_out);
        free(pipe_wr_err);
        last_pipefd = pipe_wr;
    }

    for (int i = 0; i < wait_count; i++) {
        waitpid(wait_queue[i], NULL, 0);
    }

    free(last_pipefd);

}

void sig_handler(int signo) {
    sigflag = signo;

    switch (signo) {
        case SIGINT:
            break;
        case SIGHUP:
            builtin_exit();
            break;
        default:
            break;
    }
}

int main() {
    char cwd[1024];
    // trap signals
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sig_handler;
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    char line[1024];

    char *prompt = NULL;

    while (running) {
        getcwd(cwd, sizeof(cwd));
        build_prompt(&prompt, cwd);

        printf ("%s", prompt);
        fflush (stdout);

        int status = get_line(line, sizeof(line));

        if (status == OK) {
            process_input(line);
        } else if (status == GOT_EOT) {
            builtin_exit();
        } else if (status == INTERRUPTED) {
            printf("\n");
        }

        sigflag = 0;
    }

    free(prompt);

    return 0;
}
