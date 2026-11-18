/*
 * tunnel: keep a reverse SSH tunnel from the phone to the lab host.
 *
 * The phone is behind a router that does not let anything in (its Wi-Fi network is NATed: the host
 * cannot reach the phone's address), so the phone itself opens the connection and asks the host's sshd
 * to listen on 127.0.0.1:22022 there. On the host:
 *
 *     ssh -p 22022 -i ~/.ssh/vayu_ed25519 root@127.0.0.1
 *
 * What this program runs (exactly the options the task names, then a few that only make the
 * restarting sane):
 *
 *     /var/jb/usr/bin/ssh -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=15
 *         -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
 *         -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveCountMax=3
 *         -i /var/root/.ssh/vayu_tunnel_ed25519
 *         -R 127.0.0.1:22022:127.0.0.1:22 USER@HOST
 *
 * There is no host by default: without tunnel_host= or host= (below) the program says so once and
 * does nothing more (launchd's KeepAlive would only start it again if it left).
 *
 * When it is started, and why it waits:
 *   - /var/jb/usr/bin/ssh must be there (the Procursus prefix is grafted by init, so at the first
 *     launchd jobs it may not be; day1-live.log:1754 says when it is);
 *   - some interface must have an IPv4 address (en0 by default, any non-loopback one otherwise);
 *   - a TCP connection to HOST:ssh_port must succeed. "There is a route" is not enough to know that
 *     the other end answers, and a connect is what ssh will do anyway.
 *
 * While the tunnel is up ssh is the only child; when it exits (the network went away, the far end
 * closed it, the forward could not be bound) the wait starts again after a short, growing pause.
 * launchd's KeepAlive restarts this program itself if it is killed.
 *
 * Where the values come from, lowest priority first:
 *   1. the defaults below (no host, remote 127.0.0.1:22022, local 127.0.0.1:22, no user -- ssh's own,
 *      root --, ssh /var/jb/usr/bin/ssh, key /var/root/.ssh/vayu_tunnel_ed25519);
 *   2. the kernel's boot arguments: tunnel_host=, tunnel_port=, tunnel_ssh_port=, tunnel_user=,
 *      tunnel_key=, tunnel_ssh=, tunnel_iface= (read through sysctl kern.bootargs);
 *   3. /redwood/etc/hakios-tunnel.conf, one key=value per line, # starts a comment. Same names as
 *      the boot arguments without "tunnel_": host=, port=, ssh_port=, user=, key=, ssh=, iface=,
 *      local= ("address:port"), interval=. (/etc/hakios-tunnel.conf is read first too, for a boot
 *      where this image is the root itself; in the normal boot /etc is the iOS volume's, and the
 *      image is at /redwood.) A fourth file, /var/root/hakios-tunnel.conf, is read last: it is
 *      writable from an ssh session, and what it says wins.
 *
 * Every line it writes begins with "tunnel: " and goes to stdout, which the launchd job sends to
 * /dev/console -- that is the line a run's log keeps. The one to look for is
 *
 *     tunnel: up to HOST:22022
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef CONFIG_FILE
#define CONFIG_FILE       "/redwood/etc/hakios-tunnel.conf"
#endif
#ifndef CONFIG_FILE_ALT
#define CONFIG_FILE_ALT   "/etc/hakios-tunnel.conf"  /* if a boot has this image as the root itself */
#endif
/* The writable one: made from an ssh session, read last, wins over the image's. It lives in the
 * ramfs /private/var, so it lasts until the next boot unless var_store keeps it. */
#ifndef CONFIG_FILE_VAR
#define CONFIG_FILE_VAR   "/var/root/hakios-tunnel.conf"
#endif
#define DEFAULT_HOST      ""            /* the lab host is the owner's: tunnel_host= or host= */
#define DEFAULT_PORT      22022
#define DEFAULT_SSH_PORT  22
#define DEFAULT_USER      ""            /* none: ssh's default (root) */
#define DEFAULT_SSH       "/var/jb/usr/bin/ssh"
#define DEFAULT_KEY       "/var/root/.ssh/vayu_tunnel_ed25519"
#define DEFAULT_IFACE     "en0"
#define REMOTE_LISTEN     "127.0.0.1"   /* where sshd on the host listens for us (permitlisten) */
#define LOCAL_ADDRESS     "127.0.0.1"   /* what that port is forwarded to on this phone */
#define LOCAL_PORT        22

static struct {
	char host[128];
	int port;
	int ssh_port;
	char user[64];
	char ssh[256];
	char key[256];
	char iface[32];
	char local_address[64];
	int local_port;
	int interval;                   /* seconds between checks while waiting for the network */
} config = {
	.host = DEFAULT_HOST,
	.port = DEFAULT_PORT,
	.ssh_port = DEFAULT_SSH_PORT,
	.user = DEFAULT_USER,
	.ssh = DEFAULT_SSH,
	.key = DEFAULT_KEY,
	.iface = DEFAULT_IFACE,
	.local_address = LOCAL_ADDRESS,
	.local_port = LOCAL_PORT,
	.interval = 5,
};

static volatile sig_atomic_t stopping;
static volatile pid_t ssh_pid;

static void
logmsg(const char *format, ...)
{
	va_list arguments;

	fputs("tunnel: ", stdout);
	va_start(arguments, format);
	vfprintf(stdout, format, arguments);
	va_end(arguments);
	fputc('\n', stdout);
	fflush(stdout);
}

static void
on_signal(__unused int signal_number)
{
	pid_t child = ssh_pid;

	stopping = 1;
	if (child > 0) {
		kill(child, SIGTERM);
	}
}

/* Seconds on a clock that only goes forward, with the fraction (time() is whole seconds: a 1 s
 * session could otherwise look 2 s long across a second boundary). */
static double
monotonic_seconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double) now.tv_sec + (double) now.tv_nsec / 1e9;
}

/* Sleep in one-second steps so that a SIGTERM does not wait for the whole pause. 1 if stopping. */
static int
wait_seconds(int seconds)
{
	for (int i = 0; i < seconds; i++) {
		if (stopping) {
			return 1;
		}
		sleep(1);
	}
	return stopping != 0;
}

/* ---- the values ------------------------------------------------------------------------------ */

static void
trim(char *text)
{
	char *start = text, *end;

	while (*start == ' ' || *start == '\t') {
		start++;
	}
	if (start != text) {
		memmove(text, start, strlen(start) + 1);
	}
	end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		*--end = 0;
	}
}

static const char *config_where = CONFIG_FILE;

static void
set_value(const char *name, const char *value)
{	if (strcmp(name, "host") == 0) {
		snprintf(config.host, sizeof(config.host), "%s", value);
	} else if (strcmp(name, "port") == 0) {
		config.port = atoi(value);
	} else if (strcmp(name, "ssh_port") == 0) {
		config.ssh_port = atoi(value);
	} else if (strcmp(name, "user") == 0) {
		snprintf(config.user, sizeof(config.user), "%s", value);
	} else if (strcmp(name, "ssh") == 0) {
		snprintf(config.ssh, sizeof(config.ssh), "%s", value);
	} else if (strcmp(name, "key") == 0) {
		snprintf(config.key, sizeof(config.key), "%s", value);
	} else if (strcmp(name, "iface") == 0) {
		snprintf(config.iface, sizeof(config.iface), "%s", value);
	} else if (strcmp(name, "local") == 0) {
		char address[64];
		int port;

		if (sscanf(value, "%63[^:]:%d", address, &port) == 2) {
			snprintf(config.local_address, sizeof(config.local_address), "%s", address);
			config.local_port = port;
		}
	} else if (strcmp(name, "interval") == 0) {
		config.interval = atoi(value);
	} else {
		logmsg("unknown setting \"%s\" in %s (ignored)", name, config_where);
	}
}

/* The boot arguments, as the kernel kept them (what libhakios.dylib reads: sysctl kern.bootargs). */
static void
read_boot_arguments(void)
{
	static const struct {
		const char *argument;
		const char *setting;
	} names[] = {
		{ "tunnel_host", "host" },
		{ "tunnel_port", "port" },
		{ "tunnel_ssh_port", "ssh_port" },
		{ "tunnel_user", "user" },
		{ "tunnel_key", "key" },
		{ "tunnel_ssh", "ssh" },
		{ "tunnel_iface", "iface" },
	};
	size_t size = 0;
	char *arguments, *at;

	if (sysctlbyname("kern.bootargs", NULL, &size, NULL, 0) != 0 || size < 2) {
		return;
	}
	arguments = malloc(size + 1);
	if (arguments == NULL) {
		return;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		free(arguments);
		return;
	}
	arguments[size] = 0;
	at = arguments;
	while (*at != 0) {
		char *end = strchr(at, ' ');
		char saved = 0;

		if (end != NULL) {
			saved = *end;
			*end = 0;
		}
		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			size_t length = strlen(names[i].argument);

			if (strncmp(at, names[i].argument, length) == 0 && at[length] == '=') {
				set_value(names[i].setting, at + length + 1);
			}
		}
		if (end == NULL) {
			break;
		}
		*end = saved;
		at = end + 1;
	}
	free(arguments);
}

static void
read_config_file(const char *path)
{
	FILE *file = fopen(path, "r");
	char line[512];

	config_where = path;
	if (file == NULL) {
		return;
	}
	while (fgets(line, sizeof(line), file) != NULL) {
		char *hash = strchr(line, '#'), *equals;
		char *name, *value;

		if (hash != NULL) {
			*hash = 0;
		}
		trim(line);
		if (line[0] == 0) {
			continue;
		}
		equals = strchr(line, '=');
		if (equals == NULL) {
			continue;
		}
		*equals = 0;
		name = line;
		value = equals + 1;
		trim(name);
		trim(value);
		if (name[0] != 0) {
			set_value(name, value);
		}
	}
	fclose(file);
}

/* ---- the network ----------------------------------------------------------------------------- */

/*
 * The first non-loopback IPv4 address, preferring the interface named in the configuration. On a
 * phone with two ways out this is the one that can reach the host -- which the connect below then
 * proves or disproves, whatever the address is.
 */
static int
local_ipv4(char *address, size_t length, char *name, size_t name_length)
{
	struct ifaddrs *list = NULL, *entry;
	int found = 0;

	if (getifaddrs(&list) != 0) {
		return 0;
	}
	for (int pass = 0; pass < 2 && !found; pass++) {
		for (entry = list; entry != NULL; entry = entry->ifa_next) {
			struct sockaddr_in *in;
			const char *wanted = pass == 0 ? config.iface : NULL;

			if (entry->ifa_addr == NULL || entry->ifa_addr->sa_family != AF_INET ||
			    (entry->ifa_flags & IFF_LOOPBACK) != 0) {
				continue;
			}
			if (wanted != NULL && strcmp(entry->ifa_name, wanted) != 0) {
				continue;
			}
			in = (struct sockaddr_in *) entry->ifa_addr;
			if (inet_ntop(AF_INET, &in->sin_addr, address, (socklen_t) length) == NULL) {
				continue;
			}
			snprintf(name, name_length, "%s", entry->ifa_name);
			found = 1;
			break;
		}
	}
	freeifaddrs(list);
	return found;
}

/* One TCP connect, milliseconds at most; 1 if something accepted it. */
static int
reachable(const char *host, int port, int timeout_ms)
{
	struct addrinfo hints, *addresses = NULL, *address;
	char service[16];
	int answered = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof(service), "%d", port);
	if (getaddrinfo(host, service, &hints, &addresses) != 0) {
		return 0;
	}
	for (address = addresses; address != NULL && !answered; address = address->ai_next) {
		int socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		int flags, error;
		socklen_t error_length = sizeof(error);
		struct pollfd poll_fd;

		if (socket_fd < 0) {
			continue;
		}
		flags = fcntl(socket_fd, F_GETFL, 0);
		fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
		if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
			answered = 1;
		} else if (errno == EINPROGRESS) {
			poll_fd.fd = socket_fd;
			poll_fd.events = POLLOUT;
			poll_fd.revents = 0;
			if (poll(&poll_fd, 1, timeout_ms) > 0 &&
			    getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &error_length) == 0 && error == 0) {
				answered = 1;
			}
		}
		close(socket_fd);
	}
	freeaddrinfo(addresses);
	return answered;
}

/* ---- ssh -------------------------------------------------------------------------------------- */

static int
file_ready(const char *path)
{
	struct stat status;

	return stat(path, &status) == 0 && S_ISREG(status.st_mode) && access(path, R_OK) == 0;
}

/* ssh refuses a private key anyone else may read ("UNPROTECTED PRIVATE KEY FILE"): say it first. */
static void
check_key_mode(const char *path)
{
	static int said;
	struct stat status;

	if (!said && stat(path, &status) == 0 && S_ISREG(status.st_mode) && (status.st_mode & 077) != 0) {
		said = 1;
		logmsg("warning: %s is mode %03o; ssh will refuse a key that group or others may read",
		    path, (unsigned) (status.st_mode & 0777));
	}
}

/* The exact command line, in one place: start_ssh() runs it, --dry-run prints it. */
static void
build_arguments(char **arguments, int *count_out, char *remote, size_t remote_size, char *destination,
    size_t destination_size)
{
	int count = 0;

	snprintf(remote, remote_size, "%s:%d:%s:%d", REMOTE_LISTEN, config.port, config.local_address,
	    config.local_port);
	if (config.user[0] != 0) {
		snprintf(destination, destination_size, "%s@%s", config.user, config.host);
	} else {
		snprintf(destination, destination_size, "%s", config.host);
	}

	arguments[count++] = config.ssh;
	arguments[count++] = "-N";
	arguments[count++] = "-o";
	arguments[count++] = "ExitOnForwardFailure=yes";
	arguments[count++] = "-o";
	arguments[count++] = "ServerAliveInterval=15";
	arguments[count++] = "-o";
	arguments[count++] = "StrictHostKeyChecking=no";
	arguments[count++] = "-o";
	arguments[count++] = "UserKnownHostsFile=/dev/null";
	arguments[count++] = "-o";
	arguments[count++] = "BatchMode=yes";
	arguments[count++] = "-o";
	arguments[count++] = "ConnectTimeout=15";
	arguments[count++] = "-o";
	arguments[count++] = "ServerAliveCountMax=3";
	arguments[count++] = "-o";
	arguments[count++] = "IdentitiesOnly=yes";
	arguments[count++] = "-i";
	arguments[count++] = config.key;
	arguments[count++] = "-R";
	arguments[count++] = remote;
	arguments[count++] = destination;
	arguments[count] = NULL;
	*count_out = count;
}

static pid_t
start_ssh(void)
{
	char remote[192], destination[192];
	char *arguments[24];
	int count = 0;
	pid_t child;

	build_arguments(arguments, &count, remote, sizeof(remote), destination, sizeof(destination));
	logmsg("running %s -N ... -i %s -R %s %s", config.ssh, config.key, remote, destination);
	child = fork();
	if (child < 0) {
		logmsg("fork: %s", strerror(errno));
		return -1;
	}
	if (child == 0) {
		execv(config.ssh, arguments);
		fprintf(stderr, "tunnel: exec %s: %s\n", config.ssh, strerror(errno));
		_exit(127);
	}
	ssh_pid = child;
	return child;
}

static const char *
status_text(int status, char *text, size_t length)
{
	if (WIFEXITED(status)) {
		snprintf(text, length, "exit %d", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		snprintf(text, length, "killed by signal %d", WTERMSIG(status));
	} else {
		snprintf(text, length, "status %#x", status);
	}
	return text;
}

int
main(int argc, char **argv)
{
	struct sigaction action;
	unsigned backoff = 0;
	int dry_run = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dry-run") == 0 || strcmp(argv[i], "-n") == 0) {
			dry_run = 1;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			printf("usage: tunnel [--dry-run]\n"
			    "Keeps the reverse ssh tunnel of /redwood/etc/hakios-tunnel.conf.\n"
			    "  --dry-run  print the configuration and the exact ssh command, then exit\n");
			return 0;
		}
	}

	memset(&action, 0, sizeof(action));
	action.sa_handler = on_signal;
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	signal(SIGPIPE, SIG_IGN);

	read_boot_arguments();
	read_config_file(CONFIG_FILE_ALT);       /* (if this boot has the image as its root) */
	read_config_file(CONFIG_FILE);           /* the image mounted on /redwood */
	read_config_file(CONFIG_FILE_VAR);       /* made by hand, writable: this one wins */
	if (config.port <= 0 || config.port > 65535 || config.ssh_port <= 0 || config.ssh_port > 65535) {
		logmsg("bad port in the configuration (port %d, ssh_port %d); not starting", config.port,
		    config.ssh_port);
		return 1;
	}
	if (config.interval < 1) {
		config.interval = 1;
	}
	if (config.host[0] == 0) {
		logmsg("no host: tunnel_host= among the boot arguments or host= in %s names it; nothing to do",
		    CONFIG_FILE);
		if (dry_run) {
			return 0;
		}
		while (!stopping) {
			pause();
		}
		logmsg("stopped");
		return 0;
	}
	logmsg("supervisor starting: %s%s%s:%d, -R %s:%d (the host's loopback) -> %s:%d (this phone), key %s",
	    config.user, config.user[0] != 0 ? "@" : "", config.host, config.ssh_port, REMOTE_LISTEN, config.port,
	    config.local_address, config.local_port, config.key);

	if (dry_run) {
		char remote[192], destination[192], line[1024];
		char *arguments[24];
		int count = 0;
		size_t used = 0;

		build_arguments(arguments, &count, remote, sizeof(remote), destination, sizeof(destination));
		line[0] = 0;
		for (int i = 0; i < count && used < sizeof(line); i++) {
			used += (size_t) snprintf(line + used, sizeof(line) - used, "%s%s", i == 0 ? "" : " ",
			    arguments[i]);
		}
		printf("%s\n", line);
		printf("tunnel: remote %s, host %s:%d, local %s:%d, interval %d s\n", remote, config.host,
		    config.ssh_port, config.local_address, config.local_port, config.interval);
		return 0;
	}

	while (!stopping) {
		char address[64] = "?", interface[32] = "?";
		char status[64];
		pid_t child;
		int ssh_status = 0, exited = 0;
		double started;

		if (!file_ready(config.ssh)) {
			logmsg("%s is not there yet; waiting", config.ssh);
			wait_seconds(5);
			continue;
		}
		if (!file_ready(config.key)) {
			logmsg("the key %s is not there; waiting", config.key);
			wait_seconds(5);
			continue;
		}
		check_key_mode(config.key);
		if (!local_ipv4(address, sizeof(address), interface, sizeof(interface))) {
			logmsg("no IPv4 address on any interface yet; waiting");
			wait_seconds(config.interval);
			continue;
		}
		if (!reachable(config.host, config.ssh_port, 5000)) {
			static unsigned said;

			if (said++ % 6 == 0) {
				logmsg("%s:%d is not reachable from %s (%s) yet; waiting", config.host, config.ssh_port,
				    interface, address);
			}
			wait_seconds(config.interval);
			continue;
		}
		child = start_ssh();
		if (child < 0) {
			wait_seconds(5);
			continue;
		}
		started = monotonic_seconds();
		{
			int said_up = 0;

			while (!exited) {
				pid_t result = waitpid(child, &ssh_status, WNOHANG);

				if (result == child) {
					exited = 1;
					break;
				}
				if (result < 0 && errno != EINTR) {
					ssh_status = -1;
					exited = 1;
					break;
				}
				/*
				 * ssh lives while the forward is up: ExitOnForwardFailure=yes makes it leave at once
				 * when the host will not listen on 22022, and ServerAlive* ends it within ~45 s of a
				 * dead link. Two seconds of life is therefore the sign that the host took the forward
				 * -- and the line is said then, not when the tunnel ends hours later.
				 */
				if (!said_up && monotonic_seconds() - started >= 2.0) {
					logmsg("up to %s:%d (ssh %d, from %s %s)", config.host, config.port, (int) child,
					    interface, address);
					said_up = 1;
					backoff = 0;
				}
				if (stopping) {
					kill(child, SIGTERM);
				}
				usleep(200000);
			}
		}
		ssh_pid = 0;
		if (stopping) {
			break;
		}
		if (ssh_status == -1) {
			logmsg("ssh could not be waited for: %s", strerror(errno));
		} else {
			logmsg("ssh ended (%s)", status_text(ssh_status, status, sizeof(status)));
		}
		backoff = backoff < 30 ? backoff + 3 : 30;
		logmsg("trying again in %u s", backoff);
		wait_seconds((int) backoff);
	}
	if (ssh_pid > 0) {
		kill(ssh_pid, SIGTERM);
		waitpid(ssh_pid, NULL, 0);
	}
	logmsg("stopped");
	return 0;
}
