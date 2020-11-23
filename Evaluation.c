#include "Evaluation.h"
#include "Shell.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static struct sigaction act;

static void check(bool cond, char *s)
{
    if (!cond)
    {
        perror(s);
        exit(EXIT_FAILURE);
    }
}

static void sig_handler(int sig)
{
    pid_t cpid;
    int cstat;

    while ((cpid = waitpid(-1, &cstat, WNOHANG)) > 0)
        fprintf(stderr, "Zombie process %d detected.\n", cpid);
}

int evaluer_expr(Expression *e)
{
    // Signals handling
    sigprocmask(SIG_UNBLOCK, &act.sa_mask, NULL);
    sigemptyset(&act.sa_mask);
    sigaddset(&act.sa_mask, SIGCHLD);
    act.sa_flags = 0;
    act.sa_handler = sig_handler;
    check(sigaction(SIGCHLD, &act, NULL) != -1, "Sigaction failed.");
    sigprocmask(SIG_BLOCK, &act.sa_mask, NULL);

    // Empty expression handling
    if (e->type == VIDE)
        return EXIT_SUCCESS;

    // Commands handling
    if (e->type == SIMPLE)
    {
        pid_t cpid;
        int cstat;

        cpid = fork();
        check(cpid != -1, "fork");

        if (!cpid)
        {
            if (!strcmp(e->arguments[0], "echo"))
            {
                int i = 1;
                while (e->arguments[i] != NULL)
                {
                    if (i != 1)
                        printf(" ");
                    printf("%s", e->arguments[i]);
                    i++;
                }
                printf("\n");
                exit(EXIT_SUCCESS);
            }
            else if (!strcmp(e->arguments[0], "source"))
            {
                // TODO
                fprintf(stderr, "Command %s not yet implemented.\n", e->arguments[0]);
                exit(EXIT_FAILURE);
            }
            else
            {
                execvp(e->arguments[0], e->arguments);
                exit(EXIT_FAILURE);
            }
        }

        waitpid(cpid, &cstat, 0);
        return cstat;
    }

    // Background handling
    if (e->type == BG)
    {
        pid_t cpid;

        cpid = fork();
        check(cpid != -1, "fork");

        if (!cpid)
            exit(evaluer_expr(e->gauche));

        return EXIT_SUCCESS;
    }

    // Sequences handling
    if (e->type == SEQUENCE)
    {
        evaluer_expr(e->gauche);
        return evaluer_expr(e->droite);
    }
    if (e->type == SEQUENCE_ET || e->type == SEQUENCE_OU)
    {
        int stat = evaluer_expr(e->gauche);
        if ((e->type == SEQUENCE_ET && stat == EXIT_SUCCESS) || (e->type == SEQUENCE_OU && stat != EXIT_SUCCESS))
            return evaluer_expr(e->droite);
        return stat;
    }

    // Pipelines handling
    if (e->type == PIPE)
    {
        int pipedes[2];
        pid_t cpid[2];
        int cstat[2];

        int fdstdin = dup(STDIN_FILENO);
        check(fdstdin != -1, "Could not dup stdin.");
        int fdstdout = dup(STDOUT_FILENO);
        check(fdstdout != -1, "Could not dup stdout.");

        check(pipe(pipedes) != -1, "pipe");

        cpid[0] = fork();
        check(cpid[0] != -1, "fork1");

        if (!cpid[0])
        {
            close(pipedes[0]);

            check(dup2(pipedes[1], STDOUT_FILENO) != -1, "Could not redirect stdout to pipedes[1].");
            cstat[0] = evaluer_expr(e->gauche);
            check(dup2(fdstdout, STDOUT_FILENO) != -1, "Could not redirect stdout to fdstdout.");

            close(pipedes[1]);
            exit(cstat[0]);
        }

        cpid[1] = fork();
        check(cpid[1] != -1, "fork2");

        if (!cpid[1])
        {
            close(pipedes[1]);

            check(dup2(pipedes[0], STDIN_FILENO) != -1, "Could not redirect stdin to pipedes[0].");
            cstat[1] = evaluer_expr(e->droite);
            check(dup2(fdstdin, STDIN_FILENO) != -1, "Could not redirect stdin to fdstdin.");

            close(pipedes[0]);
            exit(cstat[1]);
        }

        close(pipedes[0]);
        close(pipedes[1]);

        waitpid(cpid[0], &cstat[0], 0);
        waitpid(cpid[1], &cstat[1], 0);

        return cstat[1];
    }

    // Redirections handling
    if (e->type >= REDIRECTION_I && e->type <= REDIRECTION_EO)
    {
        int fdstdin, fdstdout, fdstderr, fd;
        int stat;

        fdstdin = dup(STDIN_FILENO);
        check(fdstdin != -1, "Could not dup stdin.");
        fdstdout = dup(STDOUT_FILENO);
        check(fdstdout != -1, "Could not dup stdout.");
        fdstderr = dup(STDERR_FILENO);
        check(fdstderr != -1, "Could not dup stderr.");

        if (e->type == REDIRECTION_I)
            fd = open(e->arguments[0], O_RDONLY);
        else if (e->type == REDIRECTION_A)
            fd = open(e->arguments[0], O_WRONLY | O_CREAT | O_APPEND, 0664);
        else
            fd = open(e->arguments[0], O_WRONLY | O_CREAT | O_TRUNC, 0664);
        check(fd != -1, "Could not open file.");

        if (e->type == REDIRECTION_I)
            check(dup2(fd, STDIN_FILENO) != -1, "Could not redirect stdin to fd.");
        if (e->type == REDIRECTION_O || e->type == REDIRECTION_A || e->type == REDIRECTION_EO)
            check(dup2(fd, STDOUT_FILENO) != -1, "Could not redirect stdout to fd.");
        if (e->type == REDIRECTION_E || e->type == REDIRECTION_EO)
            check(dup2(fd, STDERR_FILENO) != -1, "Could not redirect stderr to fd.");

        stat = evaluer_expr(e->gauche);

        if (e->type == REDIRECTION_I)
            check(dup2(fdstdin, STDIN_FILENO) != -1, "Could not redirect stdin to fdstdin.");
        if (e->type == REDIRECTION_O || e->type == REDIRECTION_A || e->type == REDIRECTION_EO)
            check(dup2(fdstdout, STDOUT_FILENO) != -1, "Could not redirect stdout to fdstdout.");
        if (e->type == REDIRECTION_E || e->type == REDIRECTION_EO)
            check(dup2(fdstderr, STDERR_FILENO) != -1, "Could not redirect stderr to fdstderr.");

        close(fdstdin);
        close(fdstdout);
        close(fdstderr);
        close(fd);
        return stat;
    }

    // Fallback
    fprintf(stderr, "Not yet implemented.\n");
    return EXIT_FAILURE;
}
