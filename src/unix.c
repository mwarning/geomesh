#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <libgen.h> /* dirname() */

#include "main.h"
#include "log.h"
#include "unix.h"


static void shutdown_handler(int signo)
{
	// exit on second stop request
	if (gstate.is_running == 0) {
		exit(1);
	}

	gstate.is_running = 0;
}

void unix_signals(void)
{
	struct sigaction sig_stop;
	struct sigaction sig_term;

	// STRG+C aka SIGINT => Stop the program
	sig_stop.sa_handler = shutdown_handler;
	sig_stop.sa_flags = 0;
	if ((sigemptyset(&sig_stop.sa_mask) == -1) || (sigaction(SIGINT, &sig_stop, NULL) != 0)) {
		log_error("Failed to set SIGINT handler: %s", strerror(errno));
		exit(1);
	}

	// SIGTERM => Stop the program gracefully
	sig_term.sa_handler = shutdown_handler;
	sig_term.sa_flags = 0;
	if ((sigemptyset(&sig_term.sa_mask) == -1) || (sigaction(SIGTERM, &sig_term, NULL) != 0)) {
		log_error("Failed to set SIGTERM handler: %s", strerror(errno));
		exit(1);
	}

	// ignore SIGPIPE
	signal(SIGPIPE, SIG_IGN);
}

void unix_fork(void)
{
	pid_t pid;
	pid_t sid;

	pid = fork();

	if (pid < 0) {
		log_error("Failed to fork: %s", strerror(errno));
		exit(1);
	} else if (pid != 0) {
		// Child process
		exit(0);
	}

	// Become session leader
	sid = setsid();
	if (sid < 0) {
		exit(1);
	}

	// Clear out the file mode creation mask
	umask(0);
}

static int is_unix_socket_valid(const char path[])
{
	struct sockaddr_un addr;
	int sock;
	int rc;

	sock = socket(AF_LOCAL, SOCK_STREAM, 0);

	addr.sun_family = AF_LOCAL;
	strcpy(addr.sun_path, path);

	rc = connect(sock, (struct sockaddr *) &addr, sizeof(addr));
	close(sock);

	return (rc == 0);
}

int unix_create_unix_socket(const char path[], int *sock_out)
{
	struct sockaddr_un addr;
	struct stat st;
	char *dir = NULL;
	int sock = -1;
	int rc;

	if (path == NULL || strlen(path) == 0) {
		goto err;
	}

	dir = dirname(strdup(path));

	if (is_unix_socket_valid(path)) {
		log_error("Socket already in use: %s", path);
		goto err;
	}

	rc = unlink(path);
	if (rc == 0) {
		log_warning("Removed stale control socket: %s", path);
	}

	// Directory does not exist and cannot be created
	if (stat(dir, &st) == -1 && mkdir(dir, 0755) != 0) {
		log_error("Cannot create directory %s %s", dir, strerror(errno));
		goto err;
	}

	sock = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (sock < 0) {
		log_error("socket(): %s", strerror(errno));
		goto err;
	}

	addr.sun_family = AF_LOCAL;
	strcpy(addr.sun_path, path);

	if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		log_error("bind() %s %s", path, strerror(errno));
		goto err;
	}

	// Try to set ugo+rwx
	chmod(path, 0777);

	listen(sock, 5);

	*sock_out = sock;

	return EXIT_SUCCESS;
err:
	if (dir) {
		free(dir);
	}

	if (sock >= 0) {
		unix_remove_unix_socket(path, sock);
	}

	return EXIT_FAILURE;
}

void unix_remove_unix_socket(const char path[], int sock_in)
{
	char *dir;

	dir = dirname(strdup(path));

	close(sock_in);
	unlink(path);
	rmdir(dir);
	free(dir);
}
