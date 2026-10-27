/*
 * mysh - a small Unix-style shell
 *
 * Features:
 *   - prompt / read / parse / execute loop
 *   - built-ins: cd, pwd, help, exit
 *   - external commands via fork + execvp
 *   - I/O redirection:  <  (stdin)   >  (stdout, truncate)   >> (stdout, append)
 *                       2> (stderr)
 *   - pipelines:        cmd1 | cmd2 | cmd3 ...
 *   - background jobs:  cmd &
 *   - quoting:          'single' and "double" quotes
 *
 * Build:  gcc -Wall -Wextra -o mysh shell.c
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_ARGS   64
#define MAX_CMDS   16
#define LINE_MAX_  1024

/* ---------- data structures ---------- */

typedef struct {
    char *argv[MAX_ARGS];   /* NULL-terminated argument vector      */
    char *in_file;          /* "< file"   (NULL if none)            */
    char *out_file;         /* "> file" or ">> file"                */
    char *err_file;         /* "2> file"                            */
    int   append;           /* 1 if ">>"                            */
} Command;

typedef struct {
    Command cmds[MAX_CMDS];
    int     ncmds;
    int     background;
} Pipeline;

/* ---------- tokenizer ---------- */

/* Splits `line` in place into tokens. Operators (| < > >> 2> &) become
 * their own tokens. Returns token count, or -1 on syntax error. */
static int tokenize(char *line, char **tokens, int max)
{
    int n = 0;
    char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        if (n >= max - 1) { fprintf(stderr, "mysh: too many tokens\n"); return -1; }

        /* operators */
        if (*p == '|' || *p == '<' || *p == '&') {
            char *t = malloc(2); t[0] = *p; t[1] = 0;
            tokens[n++] = t; p++; continue;
        }
        if (*p == '>') {
            if (p[1] == '>') { tokens[n++] = strdup(">>"); p += 2; }
            else             { tokens[n++] = strdup(">");  p += 1; }
            continue;
        }
        if (*p == '2' && p[1] == '>' && (p == line || p[-1] == ' ' || p[-1] == '\t')) {
            tokens[n++] = strdup("2>"); p += 2; continue;
        }

        /* word (with quote handling) */
        char buf[LINE_MAX_]; int len = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
               *p != '|' && *p != '<' && *p != '>' && *p != '&') {
            if (*p == '\'' || *p == '"') {
                char q = *p++;
                while (*p && *p != q) buf[len++] = *p++;
                if (!*p) { fprintf(stderr, "mysh: unterminated quote\n"); return -1; }
                p++;
            } else {
                buf[len++] = *p++;
            }
        }
        buf[len] = 0;
        tokens[n++] = strdup(buf);
    }
    tokens[n] = NULL;
    return n;
}

/* ---------- parser ---------- */

static int is_op(const char *s)
{
    return !strcmp(s, "|") || !strcmp(s, "<") || !strcmp(s, ">") ||
           !strcmp(s, ">>") || !strcmp(s, "2>") || !strcmp(s, "&");
}

/* Builds a Pipeline from tokens. Returns 0 on success, -1 on syntax error. */
static int parse(char **tokens, int n, Pipeline *pl)
{
    memset(pl, 0, sizeof *pl);
    pl->ncmds = 1;
    int argc = 0;
    Command *c = &pl->cmds[0];

    for (int i = 0; i < n; i++) {
        char *t = tokens[i];

        if (!strcmp(t, "|")) {
            if (argc == 0) { fprintf(stderr, "mysh: syntax error near '|'\n"); return -1; }
            c->argv[argc] = NULL;
            if (pl->ncmds >= MAX_CMDS) { fprintf(stderr, "mysh: pipeline too long\n"); return -1; }
            c = &pl->cmds[pl->ncmds++];
            argc = 0;
        } else if (!strcmp(t, "<") || !strcmp(t, ">") || !strcmp(t, ">>") || !strcmp(t, "2>")) {
            if (i + 1 >= n || is_op(tokens[i + 1])) {
                fprintf(stderr, "mysh: syntax error: expected filename after '%s'\n", t);
                return -1;
            }
            char *file = tokens[++i];
            if (!strcmp(t, "<"))       c->in_file = file;
            else if (!strcmp(t, "2>")) c->err_file = file;
            else { c->out_file = file; c->append = !strcmp(t, ">>"); }
        } else if (!strcmp(t, "&")) {
            if (i != n - 1) { fprintf(stderr, "mysh: '&' must be last\n"); return -1; }
            pl->background = 1;
        } else {
            if (argc >= MAX_ARGS - 1) { fprintf(stderr, "mysh: too many arguments\n"); return -1; }
            c->argv[argc++] = t;
        }
    }
    c->argv[argc] = NULL;
    if (argc == 0 && pl->ncmds > 1) { fprintf(stderr, "mysh: syntax error: empty command after '|'\n"); return -1; }
    return 0;
}

/* ---------- built-ins ---------- */

static int run_builtin(Command *c, int *handled)
{
    *handled = 1;
    char *cmd = c->argv[0];

    if (!strcmp(cmd, "exit")) {
        exit(c->argv[1] ? atoi(c->argv[1]) : 0);
    } else if (!strcmp(cmd, "cd")) {
        const char *dir = c->argv[1] ? c->argv[1] : getenv("HOME");
        if (!dir || chdir(dir) < 0) { perror("cd"); return 1; }
        return 0;
    } else if (!strcmp(cmd, "pwd")) {
        char buf[4096];
        if (getcwd(buf, sizeof buf)) puts(buf); else { perror("pwd"); return 1; }
        return 0;
    } else if (!strcmp(cmd, "help")) {
        puts("mysh built-ins: cd [dir], pwd, help, exit [code]\n"
             "Operators: cmd < in   cmd > out   cmd >> out   cmd 2> err\n"
             "           cmd1 | cmd2 | ...   cmd &");
        return 0;
    }
    *handled = 0;
    return 0;
}

/* ---------- executor ---------- */

/* Applies a command's file redirections inside a child process. */
static void apply_redirects(Command *c)
{
    if (c->in_file) {
        int fd = open(c->in_file, O_RDONLY);
        if (fd < 0) { perror(c->in_file); _exit(1); }
        dup2(fd, STDIN_FILENO); close(fd);
    }
    if (c->out_file) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->out_file, flags, 0644);
        if (fd < 0) { perror(c->out_file); _exit(1); }
        dup2(fd, STDOUT_FILENO); close(fd);
    }
    if (c->err_file) {
        int fd = open(c->err_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror(c->err_file); _exit(1); }
        dup2(fd, STDERR_FILENO); close(fd);
    }
}

static int execute(Pipeline *pl)
{
    int n = pl->ncmds;
    if (!pl->cmds[0].argv[0]) return 0;           /* empty line */

    /* A lone built-in runs in the shell itself so `cd` actually works. */
    if (n == 1) {
        int handled;
        int st = run_builtin(&pl->cmds[0], &handled);
        if (handled) return st;
    }

    fflush(stdout);                               /* don't let children inherit buffered output */

    int pipes[MAX_CMDS - 1][2];
    pid_t pids[MAX_CMDS];

    for (int i = 0; i < n - 1; i++)
        if (pipe(pipes[i]) < 0) { perror("pipe"); return 1; }

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            /* wire pipe ends first; explicit file redirects override them */
            if (i > 0)     dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < n - 1) dup2(pipes[i][1], STDOUT_FILENO);
            for (int j = 0; j < n - 1; j++) { close(pipes[j][0]); close(pipes[j][1]); }

            apply_redirects(&pl->cmds[i]);

            /* built-ins inside a pipeline run in the child */
            int handled;
            int st = run_builtin(&pl->cmds[i], &handled);
            if (handled) { fflush(stdout); _exit(st); }

            execvp(pl->cmds[i].argv[0], pl->cmds[i].argv);
            fprintf(stderr, "mysh: %s: %s\n", pl->cmds[i].argv[0], strerror(errno));
            _exit(127);
        }
        pids[i] = pid;
    }

    /* parent must close every pipe end, otherwise readers never see EOF */
    for (int i = 0; i < n - 1; i++) { close(pipes[i][0]); close(pipes[i][1]); }

    if (pl->background) {
        printf("[bg] pid %d\n", pids[n - 1]);
        return 0;
    }

    int status = 0;
    for (int i = 0; i < n; i++) {
        int st;
        waitpid(pids[i], &st, 0);
        if (i == n - 1) status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    return status;
}

/* ---------- main loop ---------- */

int main(void)
{
    char line[LINE_MAX_];
    int interactive = isatty(STDIN_FILENO);

    signal(SIGINT, SIG_IGN);                        /* Ctrl-C shouldn't kill the shell */
    signal(SIGCHLD, SIG_IGN);                       /* auto-reap background jobs */

    for (;;) {
        if (interactive) { fputs("mysh> ", stdout); fflush(stdout); }
        if (!fgets(line, sizeof line, stdin)) { if (interactive) putchar('\n'); break; }

        char *tokens[LINE_MAX_ / 2];
        int n = tokenize(line, tokens, LINE_MAX_ / 2);
        if (n > 0) {
            Pipeline pl;
            if (parse(tokens, n, &pl) == 0) execute(&pl);
            fflush(stdout);
        }
        for (int i = 0; i < (n > 0 ? n : 0); i++) free(tokens[i]);
    }
    return 0;
}
