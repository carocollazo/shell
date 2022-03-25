#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "./jobs.h"

/* Global Variables */
#define MAX_SIZE 1024 /* maximum size of buffer */
job_list_t *j_list; /* shell job list */
int j_cnt = 1;

/* Function Prototypes */
void ignore_signals();
void reap();
void parse(char *buff);
void commands(char *toks[]);
void cd(char *toks[]);
void ln(char *toks[]);
void rm(char *toks[]);
void bg(char *argv[]);
void fg(char *argv[]);
void redirection(char *toks[]);
void fork_and_exec(char *argv[], int argv_len, char *in_symbol, char *out_symbol, char *in_path, char *out_path);
void restore_signals();

int main()
{
    char buff[MAX_SIZE]; /* buffer of size 1024 */
    ssize_t r;           /* read return value */

    j_list = init_job_list();

    memset(buff, '\0', MAX_SIZE); /* instantiate buffer */

    ignore_signals(); /* ignore signals in parent */

    while (1)
    {
        // CLEAN toks argv redPaths buffer ?????
        reap(); 
#ifdef PROMPT
        const void *prompt;  /* pointer to the shell prompt "33sh> " */
        prompt = "33sh> ";

        if (write(STDOUT_FILENO, prompt, sizeof(prompt)-1) == -1)
        {
            perror("write");
            exit(EXIT_FAILURE); /* exit(1) */
        }
#endif
        
        /* if copying commandline to buffer fails */
        if ((r = read(STDIN_FILENO, buff, MAX_SIZE)) == -1)
        {
            perror("read");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        }
        /* if it reaches EOF */
        else if (!r)
        {
            cleanup_job_list(j_list);
            exit(EXIT_SUCCESS); /* exit(0) */
        }
        buff[r] = '\0'; /* set last element in buffer to null */
        parse(buff);
    }
    cleanup_job_list(j_list);
    return 1;
}

/* Helper Functions */

/* 
 * Function: ignore_signals
 * Sets signals to be ignored.
 */
void ignore_signals()
{
    /* if signal SIGINT fails */
    if (signal(SIGINT, SIG_IGN) == SIG_ERR)
        {
            perror("signal");
            exit(EXIT_FAILURE); /* exit(1) */
        }
    /* if signal SIGTSTP fails */
    if (signal(SIGTSTP, SIG_IGN) == SIG_ERR)
        {
            perror("signal");
            exit(EXIT_FAILURE); /* exit(1) */
        }
    /* if signal SIGTTOU fails */
    if (signal(SIGTTOU, SIG_IGN) == SIG_ERR)
        {
            perror("signal");
            exit(EXIT_FAILURE); /* exit(1) */
        }
}

/* 
 * Function: reap
 * Job tracking. Uses waitpid to wait for jobs to finish.
 */
void reap()
{
    int w; /* waitpid return value */
    int status;
    pid_t child_pid; /* child process ID */
    int child_jid; /* child process job ID */

    /* while the end of job list is NOT reached */
    while ((w = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
    {
        /* if waitpid fails to return process ID of the terminated child */
        if (w == -1)
        {
            perror("waitpid");
            return;
        }

        /* if waiting for any child with a process ID equal to w */
        child_pid = (pid_t)w;
        
        if ((child_jid = get_job_jid(j_list, child_pid)) < 0) {
            fprintf(stderr, "%s\n", "ERROR : get_job_jid failed."); // fprintf check for failure ???
            /* if fflush fails */
            if (fflush(stdout) < 0) {
                perror("fflush");
                cleanup_job_list(j_list);
                exit(EXIT_FAILURE);
            }
        }

        /* if process terminates normally with exit */
        if (WIFEXITED(status))
        {
            printf("[%d] (%d) terminated with exit status %d\n", child_jid, child_pid, WEXITSTATUS(status));
            /* if remove_job_jid fails */
            if (remove_job_jid(j_list, child_jid) == -1)
            {
                fprintf(stderr, "%s\n", "ERROR : remove_job_jid failed.");
                /* if fflush fails */
                if (fflush(stdout) < 0) {
                    perror("fflush");
                    cleanup_job_list(j_list);
                    exit(EXIT_FAILURE); /* exit(1) */
                }
            }
        }
        /* if process is terminated by unhandled signal */
        if (WIFSIGNALED(status))
        {
            printf("[%d] (%d) terminated by signal %d\n", child_jid, child_pid, WTERMSIG(status));
            /* if remove_job_jid fails */
            if (remove_job_jid(j_list, child_jid) == -1)
            {
                fprintf(stderr, "%s\n", "ERROR : remove_job_jid failed.");
                /* if fflush fails */
                if (fflush(stdout) < 0) {
                    perror("fflush");
                    cleanup_job_list(j_list);
                    exit(EXIT_FAILURE); /* exit(1) */
                }
            }
        }
        /* if process is stopped */
        if (WIFSTOPPED(status))
        {
            printf("[%d] (%d) suspended with exit status %d\n", child_jid, child_pid, WSTOPSIG(status));
            /* if update_job_jid fails */
            if (update_job_jid(j_list, child_jid, STOPPED) == -1)
            {
                fprintf(stderr, "%s\n", "ERROR : update_job_jid failed.");
                /* if fflush fails */
                if (fflush(stdout) < 0) {
                    perror("fflush");
                    cleanup_job_list(j_list);
                    exit(EXIT_FAILURE); /* exit(1) */
                }
            }
        }
        /* if process is resumed */
        if (WIFCONTINUED(status))
        {
            printf("[%d] (%d) resumed\n", child_jid, child_pid);
            /* if update_job_jid fails */
            if (update_job_jid(j_list, child_jid, RUNNING) == -1)
            {
                fprintf(stderr, "%s\n", "ERROR : update_job_jid failed.");
                /* if fflush fails */
                if (fflush(stdout) < 0) {
                    perror("fflush");
                    cleanup_job_list(j_list);
                    exit(EXIT_FAILURE); /* exit(1) */
                }
            }
        }
    }
    return;
}

/*
 * Function: parse
 * Parses input given by user
 * 
 * buff : pointer to buffer
 */
void parse(char *buff)
{
    char *toks[MAX_SIZE / 2];          /* array of tokens of size 512 */
    char *delim = " \t\n";             /* spaces, tabs and line breaks */
    char *token = strtok(buff, delim); /* first token (if any) in the buffer */
    int i = 0;

    memset(toks, '\0', sizeof(toks)); /* instantiate tokens */
    toks[i++] = token;
    /* if buffer is empty */
    if (token == NULL)
    {
        return;
    }

    while (1)
    {
        token = strtok(NULL, delim);
        /* if there are NO more tokens left */
        if (token == NULL)
        {
            break;
        }
        toks[i++] = token;
    }

    toks[i] = NULL; /* set last element in tokens to null */
    
    /* check for commands */
    commands(toks);
    return;
}

/* 
 * Function: commands
 * Checks if first token is a command. If it is not, calls fork_and_exec.
 * 
 * toks : pointer to tokens array
 */
void commands(char *toks[])
{
    /* BUILT IN COMMANDS */
    /* if command is change directory (cd) */
    if (!strcmp(toks[0], "cd"))
    {
        cd(toks);
    }
    /* if command is link (ln) */
    else if (!strcmp(toks[0], "ln"))
    {
        ln(toks);
        return;
    }

    /* if command is remove (rm) */
    else if (!strcmp(toks[0], "rm"))
    {
        rm(toks);
        return;
    }
    /* if command is exit */
    else if (!strcmp(toks[0], "exit"))
    {
        cleanup_job_list(j_list);
        exit(EXIT_SUCCESS); /* exit(0) */
    }

    /* NON BUILT IN COMMANDS */
    /* if command is jobs */ 
    else if (!strcmp(toks[0], "jobs"))
    {
        jobs(j_list);
        return;
    }
    /* if command is bg */
    else if (!strcmp(toks[0], "bg"))
    {
        bg(toks); 
        return;
    }
    /* if command is fg  */
    else if (!strcmp(toks[0], "fg"))
    {
        fg(toks); 
        return;
    } 
    redirection(toks);
    return;
}

/* 
 * Function: cd
 * Handles changing directory, if possible.
 * 
 * toks : pointer to tokens array
 */
void cd(char *toks[])
{
    /* if second token is null (NO directory given) */
    if (toks[1] == NULL)
    {
        fprintf(stderr, "%s\n", "SYNTAX ERROR : Change Directory (cd) failed.");
        /* if fflush fails */
        if (fflush(stdout) < 0) {
            perror("fflush");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        } 
    }
    /* if chdir fails */
    else if (chdir(toks[1]) == -1)
    {
        perror("cd");
    }
    return;
}

/* 
 * Function: ln
 * Handles linking, if possible.
 * 
 * toks : pointer to tokens array
 */
void ln(char *toks[])
{
    /* if the second or third token is null */
    if (toks[1] == NULL || toks[2] == NULL)
    {
        fprintf(stderr, "%s\n", "SYNTAX ERROR : Link (ln) failed.");
        /* if fflush fails */
        if (fflush(stdout) < 0) {
            perror("fflush");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        }
    }
    /* if link fails */
    else if (link(toks[1], toks[2]) == -1)
    {
        perror("ln");
    }
    return;
}

/* 
 * Function: rm
 * Handles removing, if possible.
 * 
 * toks : pointer to tokens array
 */
void rm(char *toks[])
{
    /* if second token is null */
    if (toks[1] == NULL)
    {
        fprintf(stderr, "%s\n", "SYNTAX ERROR : Remove (rm) failed.");
        /* if fflush fails */
        if (fflush(stdout) < 0) {
            perror("fflush");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        }
    }
    /* if unlink fails */
    else if (unlink(toks[1]) == -1)
    {
        perror("rm");
    }
    return;
}

/* 
 * Function: bg
 * If bg job, restarts job in the background.
 * 
 * toks[] : pointer to toks array
 */
void bg(char *toks[])
{
    pid_t child_pid;
    int child_jid;
    /* if the second element in toks is null */
    if (toks[1] == NULL)
    {
        fprintf(stderr, "%s\n", "SYNTAX ERROR : Background (bg) failed.");
        /* if fflush fails */
        if (fflush(stdout) < 0) {
            perror("fflush");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        }
        return;
    }
    /* if the second element in toks does NOT start with "%" */
    if (strncmp(toks[1], "%", 1))
    {
        fprintf(stderr, "%s\n", "ERROR : Inputed job does NOT begin with %.");
        /* if fflush fails */
        if (fflush(stdout) < 0) {
            perror("fflush");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        }
        return;
    }

    child_jid = atoi(toks[1] + 1); /* converts ptr to int */
    /* if get_job_pid fails */
    if ((child_pid = get_job_pid(j_list, child_jid)) == -1)
    {
        fprintf(stderr, "%s\n", "ERROR : get_job_pid failed.");
        /* if fflush fails */
        if (fflush(stdout) < 0) {
            perror("fflush");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        }
        return;
    }
    /* if kill fails */
    if (kill(-child_pid, SIGCONT) == -1)
    {
        perror("kill");
        /* if fflush fails */
        if (fflush(stdout) < 0) {
            perror("fflush");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        }
        return;
    }
    /* if kill fails */
    if (update_job_jid(j_list, child_jid, RUNNING) == -1)
    {
        fprintf(stderr, "%s\n", "ERROR : update_job_jid failed.");
        /* if fflush fails */
        if (fflush(stdout) < 0) {
            perror("fflush");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        }
        return;
    }
    return;
}

/* 
 * Function: fg
 * If fg job, restarts job in the foreground.
 * 
 * toks[] : pointer to toks array
 */
void fg(char *toks[])
{
    int w; /* waitpid return value */
    int status;
    int child_jid;
    pid_t child_pid;

    /* if the second element in toks is null */
    if (toks[1] == NULL)
    {
        fprintf(stderr, "%s\n", "SYNTAX ERROR : Foreground (fg) failed.");
        /* if fflush fails */
        if (fflush(stdout) < 0) {
            perror("fflush");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        }
        return;
    }
    /* if second element in toks does NOT start with "%" */
    if (strncmp(toks[1], "%", 1))
    {
        fprintf(stderr, "%s\n", "ERROR : Inputed job does NOT begin with %.");
        /* if fflush fails */
        if (fflush(stdout) < 0) {
            perror("fflush");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        }
        return;
    }

    child_jid = atoi(toks[1] + 1); /* converts ptr to int */ 
    /* if get_job_pid fails */
    if ((child_pid = get_job_pid(j_list, child_jid)) == -1)
    {
        fprintf(stderr, "%s\n", "ERROR : get_job_pid failed.");
        /* if fflush fails */
        if (fflush(stdout) < 0) {
            perror("fflush");
            cleanup_job_list(j_list);
            exit(EXIT_FAILURE); /* exit(1) */
        }
        return;
    }
    /* if tcsetpgrp fails */
    if (tcsetpgrp(STDIN_FILENO, child_pid) == -1)
    {
        perror("tcsetpgrp");
        return;
    }
    /* if kill to restart in fg fails */
    if (kill(-child_pid, SIGCONT) == -1)
    {
        perror("kill");
        return;
    }

    update_job_jid(j_list, child_jid, RUNNING);

    /* if waitpid fails to return process ID of the terminated child */
    if ((w = waitpid(child_pid, &status, WUNTRACED)) == -1)
    {
        perror("waitpid");
        return;
    }
    // /* if child process has no status to report */ // ???
    // if (!w)
    // {
    //     return;
    // }

    child_pid = (pid_t)w;
    
    /* if child process is terminated because of an unhandled signal */
    if (WIFSIGNALED(status))
    {
        printf("[%d] (%d) terminated by signal %d\n", child_jid, child_pid, WTERMSIG(status));
        /* if remove_job_pid fails */
        if (remove_job_pid(j_list, child_jid) == -1)
        {
            fprintf(stderr, "%s\n", "ERROR : remove_job_pid failed.");
            /* if fflush fails */
            if (fflush(stdout) < 0) 
            {
                perror("fflush");
                cleanup_job_list(j_list);
                exit(EXIT_FAILURE); /* exit(1) */
            }
        }
    }
    /* if child process is stopped */
    if (WIFSTOPPED(status))
    {
        printf("[%d] (%d) suspended by signal %d\n", child_jid, child_pid, WSTOPSIG(status));
        /* if update_job_pid fails */
        if (update_job_pid(j_list, child_pid, STOPPED) == -1)
        {
            fprintf(stderr, "%s\n", "ERROR : update_job_pid failed.");
            /* if fflush fails */
            if (fflush(stdout) < 0) 
            {
                perror("fflush");
                cleanup_job_list(j_list);
                exit(EXIT_FAILURE); /* exit(1) */
            }
        }
    }
    /* if tcsetpgrp fails */
    // if (tcsetpgrp(STDIN_FILENO, getpgrp()) == -1)
    if (tcsetpgrp(STDIN_FILENO, getpgid(0)) == -1)
    {
        perror("tcsetpgrp");
    }
    return;
}

/* 
 * Function: redirection
 * 
 * toks : pointer to tokens array
 */
void redirection(char *toks[])
{
    const char *input = "<";
    const char *output = ">";
    const char *append = ">>";
    int in_flag = 0;   /* input flag */
    int out_flag = 0;  /* output flag */
    int argv_flag = 0; /* file after redirection symbol flag */
    int argv_index = 0;
    char *in_symbol = "\0";   /* input symbol */
    char *out_symbol = "\0";  /* output symbol */
    char *in_path = "\0";     /* input path */
    char *out_path = "\0";    /* output path */
    char *argv[MAX_SIZE / 2]; /* array of size 512 */

    memset(argv, '\0', sizeof(argv)); /* instantiate argv */

    /* loop through tokens */
    for (int i = 0; i < (MAX_SIZE / 2); i++)
    {
        /* if toks[i] is NULL */
        if (toks[i] == NULL)
        {
            break;
        }
        /* if toks[i] is NOT a redirection */
        if ((strcmp(toks[i], input) != 0) && (strcmp(toks[i], output) != 0) && (strcmp(toks[i], append) != 0))
        {
            /* argv did NOT raise flag */
            if (!argv_flag)
            {
                argv[argv_index++] = toks[i];
            }
            argv_flag = 0;
        }
        /* if toks[i] is a redirection symbol */
        else
        {
            argv_flag = 1;
            /* if toks[i] is input symbol */
            if (!strcmp(toks[i], input))
            {
                /* if toks[i] is the input symbol */
                if (in_flag)
                {
                    fprintf(stderr, "%s\n", "SYNTAX ERROR : More than one input file.");
                    /* if fflush fails */
                    if (fflush(stdout) < 0) 
                    {
                        perror("fflush");
                        cleanup_job_list(j_list);
                        exit(EXIT_FAILURE); /* exit(1) */
                    }
                }
                /* if the next token is NULL (NO input file) */
                else if (toks[i + 1] == NULL)
                {
                    fprintf(stderr, "%s\n", "SYNTAX ERROR : NO input files.");
                    /* if fflush fails */
                    if (fflush(stdout) < 0) 
                    {
                        perror("fflush");
                        cleanup_job_list(j_list);
                        exit(EXIT_FAILURE); /* exit(1) */
                    }
                }
                /* if next file is the input, output or append symbol (two consecutive redirection symbols) */
                else if (!strcmp(toks[i + 1], input) || !strcmp(toks[i + 1], output) || !strcmp(toks[i + 1], append))
                {
                    fprintf(stderr, "%s\n", "SYNTAX ERROR : Input file is a redirection symbol.");
                    /* if fflush fails */
                    if (fflush(stdout) < 0) 
                    {
                        perror("fflush");
                        cleanup_job_list(j_list);
                        exit(EXIT_FAILURE); /* exit(1) */
                    }
                }

                in_flag = 1;
                in_symbol = toks[i];
                in_path = toks[i + 1];
            }
            /* if toks[i] is output or append symbol */
            if (!strcmp(toks[i], output) || !strcmp(toks[i], append))
            {
                /* if toks[i] is output symbol */
                if (out_flag)
                {
                    fprintf(stderr, "%s\n", "SYNTAX ERROR : More than one output file.");
                    /* if fflush fails */
                    if (fflush(stdout) < 0) 
                    {
                        perror("fflush");
                        cleanup_job_list(j_list);
                        exit(EXIT_FAILURE); /* exit(1) */
                    }
                }
                /* if the next token is NULL (NO output file) */
                else if (toks[i + 1] == NULL)
                {
                    fprintf(stderr, "%s\n", "SYNTAX ERROR : NO output files.");
                    /* if fflush fails */
                    if (fflush(stdout) < 0) 
                    {
                        perror("fflush");
                        cleanup_job_list(j_list);
                        exit(EXIT_FAILURE); /* exit(1) */
                    }
                }
                /* if next file is the input, output or append symbol (two consecutive redirection symbols) */
                else if (!strcmp(toks[i + 1], input) || !strcmp(toks[i + 1], output) || !strcmp(toks[i + 1], append))
                {
                    fprintf(stderr, "%s\n", "SYNTAX ERROR : Output file is a redirection symbol.");
                    /* if fflush fails */
                    if (fflush(stdout) < 0) 
                    {
                        perror("fflush");
                        cleanup_job_list(j_list);
                        exit(EXIT_FAILURE); /* exit(1) */
                    }
                }

                out_flag = 1;
                out_symbol = toks[i];
                out_path = toks[i + 1];
            }
        }
    }
    fork_and_exec(argv, argv_index, in_symbol, out_symbol, in_path, out_path);
    return;
}

/* 
 * Function: fork_and_exec
 * Handles forking, calls execv and checks if process is bg or fg
 * 
 * argv[] : pointer to argv array
 * argv_len : length of argv
 * in_symbol : pointer to input symbol
 * out_symbol : pointer to output symbol
 * in_path : pointer to input path
 * out_path : pointer to output path
 * args : pointer to arguments
 */
void fork_and_exec(char *argv[], int argv_len, char *in_symbol, char *out_symbol, char *in_path, char *out_path)
{
    int f;         /* fork return value */
    int w;         /* waitpid return value */
    int status;
    int is_bg = 0; /* background flag */

    /* if last element in argv is "&" */
    if (!strcmp(argv[argv_len - 1], "&"))
    {
        is_bg = 1;
        argv[argv_len - 1] = "\0";
    }

    /* if fork fails */
    if ((f = fork()) == -1)
    {
        perror("fork");
        return;
    }
    /* if child process is created */
    else if (!f)
    {
        restore_signals(); /* restore signals in child */
        /* if setpgid fails */
        if (setpgid(f, 0) == -1)
        {
            perror("setpgid");
            return;
        }
        
        if (!strcmp(in_symbol, "<"))
        {
            /* if close fails */
            if (close(STDIN_FILENO) == -1)
            {
                perror("close");
                return;
            }
            /* if open fails */
            if (open(in_path, O_RDONLY, 0600) == -1)
            {
                perror("open");
                return;
            }
        }
        /* if redirection is output */
        if (!strcmp(out_symbol, ">"))
        {
            /* if close fails */
            if (close(STDOUT_FILENO) == -1)
            {
                perror("close");
                return;
            }
            /* if open fails */
            if (open(out_path, O_RDWR | O_CREAT | O_TRUNC, 0600) == -1)
            {
                perror("open");
                return;
            }
        }
        /* if redirection is append */
        if (!strcmp(out_symbol, ">>"))
        {
            /* if close fails */
            if (close(STDOUT_FILENO) == -1)
            {
                perror("close");
                return;
            }
            /* if close fails */
            if (open(out_path, O_RDWR | O_CREAT | O_APPEND, 0600) == -1)
            {
                perror("open");
                return;
            }
        }
    
        // argv[0] = strrchr(argv[0], atoi("/")) + 1; /* find pointer to first non "/" character and store as first element of argv */ // ????
        
        /* if execv fails */
        if (execv(argv[0], argv) == -1)
        {
            perror("execv");
            return;
        }
    }
    /* if child is background process */
    if (is_bg)
    {
        if (add_job(j_list, j_cnt, f, RUNNING, argv[0]) == -1) 
        {
            fprintf(stderr, "%s\n", "ERROR : add_job failed.");
            if (fflush(stdout) < 0) 
            {
                perror("fflush");
                return;
            }
        }
        printf("[%d] (%d)\n", j_cnt, f);
        j_cnt++;
    } /* if child is foreground process */
    else 
    { 
        /* if waitpid fails */
        if ((w = waitpid(f, &status, WUNTRACED)) == -1) {
            perror("waitpid");
            return;
        }
        /* if process is terminated by unhandled signal */
        if (WIFSIGNALED(status))
        {
            printf("[%d] (%d) terminated by signal %d\n", j_cnt, f, WTERMSIG(status));
        }
        /* if process is stopped */
        if (WIFSTOPPED(status))
        {
            printf("[%d] (%d) suspended with exit status %d\n", j_cnt, f, WSTOPSIG(status));
            /* if add_job fails */
            if (add_job(j_list, j_cnt, w, STOPPED, argv[0]) == -1)
            {
                fprintf(stderr, "%s\n", "ERROR : add_job failed.");
                /* if fflush fails */
                if (fflush(stdout) < 0) {
                    perror("fflush");
                    return;
                }
            }
            j_cnt++;
        }
        /* if tcsetpgrp fails */
        if (tcsetpgrp(STDIN_FILENO, getpgid(0)) == -1) // getpgrp ???
        {
            perror("tcsetpgrp");
            return;
        }
    }
    return;
}

/* 
 * Function: restore_signals
 * Restores signals
 */
void restore_signals()
{
    /* if signal SIGINT fails */
    if (signal(SIGINT, SIG_DFL) == SIG_ERR)
    {
        perror("signal");
        exit(EXIT_FAILURE); /* exit(1) */
    }
    /* if signal SIGTSTP fails */
    if (signal(SIGTSTP, SIG_DFL) == SIG_ERR)
    {
        perror("signal");
        exit(EXIT_FAILURE); /* exit(1) */
    }
    /* if signal SIGTTOU fails */
    if (signal(SIGTTOU, SIG_DFL) == SIG_ERR)
    {
        perror("signal");
        exit(EXIT_FAILURE); /* exit(1) */
    }
}
