/*
 * minish: the shell usbctld gives to "oem sh".
 *
 * iOS 16.5's own root has no shell at all -- /bin is df and ps and nothing else (the comment at
 * the top of userland/build.sh says so, and a flash-root boot answers "/bin/sh: No such file or
 * directory", status 127).  The jailbreak prefix, when the image carries one, has /var/jb/bin/sh
 * and usbctld uses it first; this file is the fallback that makes the channel work on an image
 * without one, and it is also what the host test (host/test/) runs.
 *
 * What it understands -- enough for the commands the host scripts send:
 *
 *   a ; b        one after the other            a && b / a || b   by the exit status
 *   a | b        a pipe, any number of stages    '...' "..." \x    quoting
 *   > >> < 2> 2>&1 1>&2  redirection, with an optional descriptor number
 *
 * Built in (they need no /usr/bin): echo, uname, ps, wc, cat, sleep, pwd, true, false, exit,
 * help.  Anything else is exec'd as it is (with a '/' in it) or looked for in PATH.
 *
 * Not there: variables, globbing, subshells, functions, job control, here-documents.  A line
 * that uses them fails as a command that is not found.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <dirent.h>
#include <sys/utsname.h>
#endif

#include "minish.h"

#define MAX_WORDS       256
#define MAX_STAGES      16
#define REDIR_FROM_FILE (-3)    /* c->redir[fd]: open c->redir_file[fd] for this descriptor */

struct command {
	char *argv[MAX_WORDS + 1];
	int   argc;
	int   redir[8];                 /* -2 untouched, -1 closed, >= 0 dup from, REDIR_FROM_FILE */
	char *redir_file[8];
	int   redir_append[8];
};

struct pipeline {
	struct command stage[MAX_STAGES];
	int   nstages;
	int   connector;                /* 0 end, 1 ';' or '&', 2 "&&", 3 "||" */
};

static const char *minish_path =
    "/usr/local/bin:/var/jb/usr/local/bin:/usr/bin:/var/jb/usr/bin:/bin:/var/jb/bin:"
    "/usr/sbin:/var/jb/usr/sbin:/sbin:/var/jb/sbin:/redwood/bin";

/* ---------------------------------------------------------------------------------- words */

/*
 * One word, from *pp.  Quotes are taken off and a backslash escapes the next character outside
 * single quotes.  The unquoted text goes into word_arena, which minish_run() resets for every
 * line: a word can only get shorter than its text, and the words have to outlive the parse (they
 * are passed to the children).
 */
static char   word_arena[32 * 1024];
static size_t word_arena_used;
static int    last_status;             /* what "$?" stands for */

static int
next_word(char **pp, char **out)
{
	char *p = *pp, *w;

	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p == 0 || *p == '\n' || *p == ';' || *p == '|' || *p == '&' || *p == '<' || *p == '>') {
		*pp = p;
		return 0;
	}
	if (word_arena_used + 2 >= sizeof(word_arena)) {
		return 0;
	}
	w = word_arena + word_arena_used;
	*out = w;
	while (*p != 0 && *p != ' ' && *p != '\t' && *p != '\n' && *p != ';' && *p != '|' && *p != '&' &&
	    *p != '<' && *p != '>') {
		if (*p == '\'') {
			p++;
			while (*p != 0 && *p != '\'') {
				*w++ = *p++;
			}
			if (*p == '\'') {
				p++;
			}
		} else if (*p == '"') {
			p++;
			while (*p != 0 && *p != '"') {
				if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$' || p[1] == '`')) {
					p++;
					*w++ = *p++;
					continue;
				}
				if (*p == '$' && p[1] == '?') {
					char number[16];
					int n = snprintf(number, sizeof(number), "%d", last_status);

					p += 2;
					for (int k = 0; k < n && (size_t)(w - word_arena) + 2 < sizeof(word_arena); k++) {
						*w++ = number[k];
					}
					continue;
				}
				*w++ = *p++;
			}
			if (*p == '"') {
				p++;
			}
		} else if (*p == '$' && p[1] == '?') {
			p += 2;
			{
				char number[16];
				int n = snprintf(number, sizeof(number), "%d", last_status);

				for (int k = 0; k < n && (size_t)(w - word_arena) + 2 < sizeof(word_arena); k++) {
					*w++ = number[k];
				}
			}
		} else if (*p == '\\' && p[1] != 0) {
			p++;
			*w++ = *p++;
		} else {
			*w++ = *p++;
		}
		if ((size_t)(w - word_arena) + 2 >= sizeof(word_arena)) {
			break;
		}
	}
	*w = 0;
	word_arena_used = (size_t)(w - word_arena) + 1;
	*pp = p;
	return 1;
}

/* The redirection at p, if any: its length, the descriptor, the kind (0 <, 1 >, 2 >>, 3 dup). */
static int
redir_at(const char *p, int *fd, int *kind, int *dupfrom)
{
	int n = 0, d = -1;

	if (p[0] >= '0' && p[0] <= '9' && (p[1] == '>' || p[1] == '<')) {
		d = p[0] - '0';
		n = 1;
	}
	if (p[n] == '<') {
		*fd = d >= 0 ? d : 0;
		*kind = 0;
		return n + 1;
	}
	if (p[n] == '>') {
		if (p[n + 1] == '>') {
			*fd = d >= 0 ? d : 1;
			*kind = 2;
			return n + 2;
		}
		if (p[n + 1] == '&' && p[n + 2] >= '0' && p[n + 2] <= '9') {
			*fd = d >= 0 ? d : 1;
			*kind = 3;
			*dupfrom = p[n + 2] - '0';
			return n + 3;
		}
		*fd = d >= 0 ? d : 1;
		*kind = 1;
		return n + 1;
	}
	return 0;
}

static int
parse_pipeline(char **pp, struct pipeline *pl)
{
	char *p = *pp;

	memset(pl, 0, sizeof(*pl));
	for (;;) {
		struct command *c = &pl->stage[pl->nstages];
		char *word;

		c->argc = 0;
		for (int i = 0; i < 8; i++) {
			c->redir[i] = -2;
		}
		for (;;) {
			int fd, kind = 0, dupfrom = -1, len;

			while (*p == ' ' || *p == '\t') {
				p++;
			}
			if (*p == 0 || *p == '\n' || *p == ';' || *p == '|' || *p == '&') {
				break;
			}
			len = redir_at(p, &fd, &kind, &dupfrom);
			if (len != 0) {
				p += len;
				if (kind == 3) {
					c->redir[fd] = dupfrom;
					continue;
				}
				while (*p == ' ' || *p == '\t') {
					p++;
				}
				if (!next_word(&p, &word)) {
					return -1;
				}
				c->redir_file[fd] = word;
				c->redir_append[fd] = kind == 2;
				c->redir[fd] = REDIR_FROM_FILE;
				continue;
			}
			if (!next_word(&p, &word)) {
				break;
			}
			if (c->argc < MAX_WORDS) {
				c->argv[c->argc++] = word;
			}
		}
		c->argv[c->argc] = NULL;
		pl->nstages++;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '|' && p[1] != '|') {
			p++;
			if (pl->nstages >= MAX_STAGES) {
				return -1;
			}
			continue;
		}
		break;
	}
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p == '&' && p[1] == '&') {
		pl->connector = 2;
		p += 2;
	} else if (*p == '|' && p[1] == '|') {
		pl->connector = 3;
		p += 2;
	} else if (*p == ';' || *p == '&') {
		pl->connector = 1;
		p++;
	} else {
		pl->connector = 0;
	}
	*pp = p;
	return 0;
}

/* ------------------------------------------------------------------------------- builtins */

static int
bi_echo(struct command *c)
{
	int newline = 1, first = 1;

	if (c->argc > 1 && strcmp(c->argv[1], "-n") == 0) {
		newline = 0;
	}
	for (int i = newline ? 1 : 2; i < c->argc; i++) {
		if (!first) {
			fputc(' ', stdout);
		}
		fputs(c->argv[i], stdout);
		first = 0;
	}
	if (newline) {
		fputc('\n', stdout);
	}
	return 0;
}

static int
bi_uname(struct command *c)
{
#ifdef __APPLE__
	struct utsname u;

	if (uname(&u) != 0) {
		perror("uname");
		return 1;
	}
	if (c->argc == 1) {
		printf("%s\n", u.sysname);
		return 0;
	}
	for (int i = 1; i < c->argc; i++) {
		const char *f = c->argv[i];

		if (strcmp(f, "-a") == 0) {
			printf("%s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
			return 0;
		} else if (strcmp(f, "-s") == 0) {
			printf("%s\n", u.sysname);
		} else if (strcmp(f, "-n") == 0) {
			printf("%s\n", u.nodename);
		} else if (strcmp(f, "-r") == 0) {
			printf("%s\n", u.release);
		} else if (strcmp(f, "-v") == 0) {
			printf("%s\n", u.version);
		} else if (strcmp(f, "-m") == 0 || strcmp(f, "-p") == 0) {
			printf("%s\n", u.machine);
		} else {
			fprintf(stderr, "uname: unknown option %s\n", f);
			return 1;
		}
	}
	return 0;
#else
	struct utsname u;

	if (uname(&u) != 0) {
		perror("uname");
		return 1;
	}
	if (c->argc > 1 && strcmp(c->argv[1], "-a") == 0) {
		printf("%s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
	} else {
		printf("%s\n", u.sysname);
	}
	return 0;
#endif
}

static int
bi_ps(struct command *c)
{
	(void)c;
	printf("  PID COMMAND\n");
#ifdef __APPLE__
	{
		int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
		size_t size = 0;
		char *buf = NULL;
		struct kinfo_proc *procs;
		int count;

		if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) {
			perror("ps: sysctl");
			return 1;
		}
		size += 16 * sizeof(struct kinfo_proc);
		buf = malloc(size);
		if (buf == NULL) {
			return 1;
		}
		if (sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
			perror("ps: sysctl");
			free(buf);
			return 1;
		}
		count = (int)(size / sizeof(struct kinfo_proc));
		procs = (struct kinfo_proc *)buf;
		for (int i = 0; i < count; i++) {
			const char *name = procs[i].kp_proc.p_comm;

			printf("%5d %s\n", procs[i].kp_proc.p_pid, name[0] != 0 ? name : "?");
		}
		free(buf);
	}
#else
	{
		DIR *d = opendir("/proc");
		struct dirent *e;

		if (d == NULL) {
			perror("ps: /proc");
			return 1;
		}
		while ((e = readdir(d)) != NULL) {
			char path[300], name[64] = "?";
			FILE *f;

			if (!isdigit((unsigned char)e->d_name[0])) {
				continue;
			}
			snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
			if ((f = fopen(path, "r")) != NULL) {
				if (fgets(name, sizeof(name), f) != NULL) {
					char *nl = strchr(name, '\n');

					if (nl != NULL) {
						*nl = 0;
					}
				}
				fclose(f);
			}
			printf("%5d %s\n", atoi(e->d_name), name);
		}
		closedir(d);
	}
#endif
	return 0;
}

static int
bi_wc(struct command *c)
{
	int do_lines = 0, do_words = 0, do_bytes = 0, first_file = c->argc;

	for (int i = 1; i < c->argc; i++) {
		if (strcmp(c->argv[i], "-l") == 0) {
			do_lines = 1;
		} else if (strcmp(c->argv[i], "-w") == 0) {
			do_words = 1;
		} else if (strcmp(c->argv[i], "-c") == 0) {
			do_bytes = 1;
		} else {
			first_file = i;
			break;
		}
	}
	if (!do_lines && !do_words && !do_bytes) {
		do_lines = do_words = do_bytes = 1;
	}
	for (int f = first_file; f <= c->argc; f++) {
		FILE *in = stdin;
		int l = 0, w = 0, b = 0, inword = 0, ch;

		if (f < c->argc) {
			if ((in = fopen(c->argv[f], "r")) == NULL) {
				fprintf(stderr, "wc: %s: %s\n", c->argv[f], strerror(errno));
				return 1;
			}
		}
		while ((ch = fgetc(in)) != EOF) {
			b++;
			if (ch == '\n') {
				l++;
			}
			if (isspace(ch)) {
				inword = 0;
			} else if (!inword) {
				inword = 1;
				w++;
			}
		}
		if (in != stdin) {
			fclose(in);
		}
		if (do_lines) {
			printf("%7d", l);
		}
		if (do_words) {
			printf("%7d", w);
		}
		if (do_bytes) {
			printf("%7d", b);
		}
		if (f < c->argc) {
			printf(" %s\n", c->argv[f]);
		} else {
			printf("\n");
		}
	}
	return 0;
}

static int
bi_cat(struct command *c)
{
	int status = 0;

	if (c->argc == 1) {
		char buf[8192];
		size_t n;

		while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
			fwrite(buf, 1, n, stdout);
		}
		return 0;
	}
	for (int i = 1; i < c->argc; i++) {
		FILE *f = fopen(c->argv[i], "r");
		char buf[8192];
		size_t n;

		if (f == NULL) {
			fprintf(stderr, "cat: %s: %s\n", c->argv[i], strerror(errno));
			status = 1;
			continue;
		}
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
			fwrite(buf, 1, n, stdout);
		}
		fclose(f);
	}
	return status;
}

/* "printf 'status\ncalls\n' > /dev/vayu-voice": %s %d %% and the escapes, which is all the
 * host scripts ask of it. */
static int
bi_printf(struct command *c)
{
	const char *p;
	int arg = 2;

	if (c->argc < 2) {
		return 0;
	}
	for (p = c->argv[1]; *p != 0; p++) {
		if (*p == '\\' && p[1] != 0) {
			p++;
			switch (*p) {
			case 'n': fputc('\n', stdout); break;
			case 't': fputc('\t', stdout); break;
			case 'r': fputc('\r', stdout); break;
			default:  fputc(*p, stdout); break;
			}
			continue;
		}
		if (*p == '%' && p[1] != 0) {
			p++;
			if (*p == '%') {
				fputc('%', stdout);
			} else if (*p == 's') {
				if (arg < c->argc) {
					fputs(c->argv[arg++], stdout);
				}
			} else if (*p == 'd') {
				if (arg < c->argc) {
					printf("%d", atoi(c->argv[arg++]));
				}
			} else {
				fputc('%', stdout);
				fputc(*p, stdout);
			}
			continue;
		}
		fputc(*p, stdout);
	}
	return 0;
}

static int
bi_sleep(struct command *c)
{
	double seconds;
	struct timespec ts;

	if (c->argc < 2) {
		fprintf(stderr, "sleep: how long?\n");
		return 1;
	}
	seconds = atof(c->argv[1]);
	if (seconds < 0) {
		seconds = 0;
	}
	ts.tv_sec = (time_t)seconds;
	ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
	return 0;
}

struct builtin {
	const char *name;
	int (*fn)(struct command *);
};

static const struct builtin builtins[] = {
	{ "echo", bi_echo }, { "uname", bi_uname }, { "ps", bi_ps }, { "wc", bi_wc },
	{ "cat", bi_cat }, { "sleep", bi_sleep }, { "printf", bi_printf }, { NULL, NULL },
};

static int
bi_help(struct command *c)
{
	(void)c;
	printf("minish: ; && || | > >> < 2>&1, and: echo uname ps wc cat sleep printf pwd true false exit help\n");
	printf("        anything else is run from PATH (%s)\n", minish_path);
	return 0;
}

static int
is_builtin(const char *name)
{
	for (int i = 0; builtins[i].name != NULL; i++) {
		if (strcmp(name, builtins[i].name) == 0) {
			return i;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------------------- execution */

static void
apply_redirections(struct command *c)
{
	for (int fd = 0; fd < 8; fd++) {
		if (c->redir[fd] == -2) {
			continue;
		}
		if (c->redir[fd] == -1) {
			close(fd);
			continue;
		}
		if (c->redir[fd] >= 0) {
			if (dup2(c->redir[fd], fd) < 0) {
				perror("dup2");
				_exit(1);
			}
			continue;
		}
		if (c->redir[fd] == REDIR_FROM_FILE && c->redir_file[fd] != NULL) {
			int flags = fd == 0 ? O_RDONLY : (O_WRONLY | O_CREAT | (c->redir_append[fd] ? O_APPEND : O_TRUNC));
			int f = open(c->redir_file[fd], flags, 0644);

			if (f < 0) {
				fprintf(stderr, "sh: %s: %s\n", c->redir_file[fd], strerror(errno));
				_exit(1);
			}
			if (dup2(f, fd) < 0) {
				perror("dup2");
				_exit(1);
			}
			close(f);
		}
	}
}

static void
exec_external(struct command *c)
{
	extern char **environ;
	const char *name = c->argv[0];

	if (strchr(name, '/') != NULL) {
		execve(name, c->argv, environ);
	} else {
		char path[1024];
		const char *p = minish_path;

		while (*p != 0) {
			const char *colon = strchr(p, ':');
			size_t len = colon != NULL ? (size_t)(colon - p) : strlen(p);

			if (len + 1 + strlen(name) + 1 <= sizeof(path)) {
				memcpy(path, p, len);
				path[len] = '/';
				strcpy(path + len + 1, name);
				execve(path, c->argv, environ);
			}
			if (colon == NULL) {
				break;
			}
			p = colon + 1;
		}
	}
	fprintf(stderr, "sh: %s: %s\n", name, strerror(errno));
	_exit(errno == ENOENT ? 127 : 126);
}

/* A command, in a child of its own: a pipeline stage and a redirection then take the same code. */
static void
run_simple_in_child(struct command *c)
{
	int b;

	apply_redirections(c);
	if (c->argc == 0) {
		_exit(0);
	}
	if (strcmp(c->argv[0], "true") == 0) {
		_exit(0);
	}
	if (strcmp(c->argv[0], "false") == 0) {
		_exit(1);
	}
	if (strcmp(c->argv[0], "pwd") == 0) {
		char buf[1024];

		if (getcwd(buf, sizeof(buf)) != NULL) {
			printf("%s\n", buf);
		}
		_exit(0);
	}
	if (strcmp(c->argv[0], "help") == 0) {
		_exit(bi_help(c));
	}
	if (strcmp(c->argv[0], "exit") == 0) {
		_exit(c->argc > 1 ? atoi(c->argv[1]) & 0xff : 0);
	}
	b = is_builtin(c->argv[0]);
	if (b >= 0) {
		int st = builtins[b].fn(c);

		fflush(stdout);
		_exit(st & 0xff);
	}
	exec_external(c);
}

static int
execute_pipeline(struct pipeline *pl)
{
	int pids[MAX_STAGES];
	int prev = -1;
	int status = 0;

	for (int i = 0; i < pl->nstages; i++) {
		int fds[2] = { -1, -1 };
		pid_t pid;

		if (i + 1 < pl->nstages && pipe(fds) != 0) {
			perror("pipe");
			return 1;
		}
		pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0) {
			if (prev >= 0) {
				dup2(prev, 0);
				close(prev);
			}
			if (fds[1] >= 0) {
				dup2(fds[1], 1);
				close(fds[1]);
			}
			if (fds[0] >= 0) {
				close(fds[0]);
			}
			run_simple_in_child(&pl->stage[i]);
			_exit(127);
		}
		if (prev >= 0) {
			close(prev);
		}
		if (fds[1] >= 0) {
			close(fds[1]);
		}
		prev = fds[0];
		pids[i] = pid;
	}
	if (prev >= 0) {
		close(prev);
	}
	for (int i = 0; i < pl->nstages; i++) {
		int st = 0;

		while (waitpid(pids[i], &st, 0) < 0 && errno == EINTR) {
		}
		if (i == pl->nstages - 1) {
			if (WIFEXITED(st)) {
				status = WEXITSTATUS(st);
			} else if (WIFSIGNALED(st)) {
				status = 128 + WTERMSIG(st);
			} else {
				status = 1;
			}
		}
	}
	fflush(stdout);
	return status;
}

int
minish_run(const char *line)
{
	char *copy = strdup(line);
	char *p;
	int status = 0, run = 1;

	if (copy == NULL) {
		return 1;
	}
	p = copy;
	word_arena_used = 0;
	for (;;) {
		struct pipeline pl;

		while (*p == ' ' || *p == '\t' || *p == '\n') {
			p++;
		}
		if (*p == 0) {
			break;
		}
		if (parse_pipeline(&p, &pl) != 0) {
			fprintf(stderr, "sh: cannot parse that line\n");
			status = 2;
			break;
		}
		if (pl.nstages == 1 && pl.stage[0].argc == 0) {
			break;
		}
		if (run) {
			status = execute_pipeline(&pl);
			last_status = status;
		}
		if (pl.connector == 0) {
			break;
		}
		if (pl.connector == 1) {
			run = 1;
		} else if (pl.connector == 2) {
			run = status == 0;
		} else {
			run = status != 0;
		}
	}
	free(copy);
	return status;
}

int
minish_main(int argc, char **argv)
{
	if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
		return minish_run(argv[2]);
	}
	if (argc == 2) {
		FILE *f = fopen(argv[1], "r");
		char line[4096];
		int status = 0;

		if (f == NULL) {
			fprintf(stderr, "sh: %s: %s\n", argv[1], strerror(errno));
			return 127;
		}
		while (fgets(line, sizeof(line), f) != NULL) {
			status = minish_run(line);
		}
		fclose(f);
		return status;
	}
	{
		char line[4096];

		while (fgets(line, sizeof(line), stdin) != NULL) {
			minish_run(line);
		}
	}
	return 0;
}
