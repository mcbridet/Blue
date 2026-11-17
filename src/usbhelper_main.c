/*
 * blue-usb-helper (macOS): usbhelper.h's helper, as a program. Blue starts
 * it as root (usbremote.c), its end of their socket its standard input and
 * output; it's of no use otherwise.
 */
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "usbhelper.h"

extern char **environ;

static void quit(int sig)
{
	(void)sig;
	usbhelper_quit = 1;
}

/* Every file it was given but the socket (and standard input, output and error) */
static void close_others(int keep)
{
	static struct proc_fdinfo fds[4096];
	int n = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, fds, sizeof fds);
	for (int k = 0; k < n / (int)sizeof *fds; k++)
		if (fds[k].proc_fd > 2 && fds[k].proc_fd != keep)
			close(fds[k].proc_fd);
}

int main(int argc, char **argv)
{
	struct stat st;
	if (argc != 2 || strcmp(argv[1], "--serve") || fstat(0, &st) != 0 || !S_ISSOCK(st.st_mode)) {
		fprintf(stderr, "blue-usb-helper: Blue's USB helper, which Blue starts when it needs it\n");
		return 2;
	}
	if (geteuid() == 0 || issetugid()) {
		static char *none[] = { NULL };
		environ = none;         /* (nothing of the user's as root: not BLUE_LIBUSB, say) */
		if (geteuid() == 0 && setuid(0) != 0)
			return 2;
	}
	int fd = fcntl(0, F_DUPFD_CLOEXEC, 3), null = open("/dev/null", O_RDWR);
	if (fd < 0 || null < 0)
		return 2;
	dup2(null, 0);
	if (fcntl(2, F_GETFD) < 0)
		dup2(null, 2);
	dup2(2, 1);                     /* (what usbhost.c prints goes with Blue's messages, not into the socket) */
	close_others(fd);
	if (chdir("/") != 0)
		return 2;
	umask(077);
	setsid();                       /* (the terminal's ^C and ^Z are Blue's: it ends this) */
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = quit;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
	return usbhelper_serve(fd);
}
