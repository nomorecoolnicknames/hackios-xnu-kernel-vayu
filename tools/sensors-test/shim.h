/*
 * Included before sensors.c or motion.c (cc -include): the system headers first, then the calls the
 * tests take over -- the clock, sleeping, the devices, the log -- renamed to the fakes in fake.h, and
 * the Darwin spellings glibc does not have.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define __unused                        __attribute__((unused))
#define pthread_setname_np(name)        ((void)(name))
#define open                            fake_open
#define read                            fake_read
#define write                           fake_write
#define close                           fake_close
#define access                          fake_access
#define sleep                           fake_sleep
#define usleep                          fake_usleep

int fake_open(const char *path, int flags, ...);
ssize_t fake_read(int fd, void *buffer, size_t size);
ssize_t fake_write(int fd, const void *buffer, size_t size);
int fake_close(int fd);
int fake_access(const char *path, int mode);
unsigned fake_sleep(unsigned seconds);
int fake_usleep(useconds_t us);
