#ifndef MINISH_H
#define MINISH_H

/* A small shell for usbctld: see minish.c.  Returns the exit status of the last command. */
int minish_run(const char *line);

/* argv as a program would see it: {"sh", "-c", "line"} or {"sh", "script"}. */
int minish_main(int argc, char **argv);

#endif
