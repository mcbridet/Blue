#include "chooser.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct chooser {
	pthread_mutex_t lock;
	enum chooser_state state;
	char *path;                     /* (chosen) */
	bool closed;                    /* closed unfinished: it goes once it's done */
	void (*wake)(void *);
	void *arg;
	void *panel;                    /* macOS's NSOpenPanel, */
	pid_t pid;                      /* ... or zenity's or kdialog's process */
	int out;                        /* ... and its standard output */
};

struct chooser *chooser_new(void (*wake)(void *), void *arg, void *panel)
{
	struct chooser *c = calloc(1, sizeof *c);
	if (!c)
		return NULL;
	pthread_mutex_init(&c->lock, NULL);
	c->wake = wake;
	c->arg = arg;
	c->panel = panel;
	c->pid = -1;
	c->out = -1;
	return c;
}

static void chooser_free(struct chooser *c)
{
	pthread_mutex_destroy(&c->lock);
	free(c->path);
	free(c);
}

void chooser_done(struct chooser *c, enum chooser_state state, char *path)
{
	pthread_mutex_lock(&c->lock);
	if (c->closed) {
		pthread_mutex_unlock(&c->lock);
		free(path);
		chooser_free(c);
		return;
	}
	c->state = state;
	c->path = path;
	if (c->wake)
		c->wake(c->arg);        /* (under the lock: once chooser_close() has it, there's no more waking) */
	pthread_mutex_unlock(&c->lock);
}

enum chooser_state chooser_poll(struct chooser *c, char **path)
{
	pthread_mutex_lock(&c->lock);
	enum chooser_state state = c->state;
	pthread_mutex_unlock(&c->lock);
	*path = NULL;
	if (state == CHOOSER_WAITING)
		return state;
	*path = c->path;
	c->path = NULL;
	chooser_free(c);
	return state;
}

void chooser_close(struct chooser *c)
{
	pthread_mutex_lock(&c->lock);
	if (c->state != CHOOSER_WAITING) {
		pthread_mutex_unlock(&c->lock);
		chooser_free(c);
		return;
	}
	c->closed = true;
	void *panel = c->panel;
	if (c->pid > 0)
		kill(c->pid, SIGTERM);
	pthread_mutex_unlock(&c->lock);
#ifdef __APPLE__
	if (panel)
		chooser_cancel_panel(panel);    /* (its handler, on this thread, then frees it) */
#else
	(void)panel;
#endif
}

/* ---- zenity's or kdialog's ----------------------------------------------------------- */

/* A program on the PATH: its path in `out` */
static bool on_path(const char *name, char *out, size_t len)
{
	const char *p = getenv("PATH");
	while (p && *p) {
		const char *colon = strchr(p, ':');
		size_t n = colon ? (size_t)(colon - p) : strlen(p);
		if (n && n + strlen(name) + 2 <= len) {
			snprintf(out, len, "%.*s/%s", (int)n, p, name);
			if (access(out, X_OK) == 0)
				return true;
		}
		p += n + (colon != NULL);
	}
	return false;
}

/* Its thread: what it prints is the file chosen; it says it was cancelled by exiting with 1 */
static void *watch(void *arg)
{
	struct chooser *c = arg;
	char buf[4096], part[512];
	size_t len = 0;
	bool whole = true;
	for (;;) {
		ssize_t n = read(c->out, part, sizeof part);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (len + (size_t)n < sizeof buf) {
			memcpy(buf + len, part, (size_t)n);
			len += (size_t)n;
		} else {
			whole = false;
		}
	}
	close(c->out);
	int status = 0;
	while (waitpid(c->pid, &status, 0) < 0 && errno == EINTR)
		;
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		len--;
	buf[len] = '\0';
	char *path = NULL;
	enum chooser_state state = CHOOSER_FAILED;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && len && whole && (path = strdup(buf)))
		state = CHOOSER_CHOSEN;
	else if ((WIFEXITED(status) && WEXITSTATUS(status) == 1) || WIFSIGNALED(status))
		state = CHOOSER_CANCELLED;
	chooser_done(c, state, path);
	return NULL;
}

struct chooser *chooser_spawn(const char *message, void (*wake)(void *), void *arg)
{
	char prog[1024], title[256], start[1024];
	const char *home = getenv("HOME");
	snprintf(title, sizeof title, "%s", message);
	snprintf(start, sizeof start, "%s/", home ? home : ".");
	char *zenity[] = { prog, "--file-selection", "--title", title, NULL };
	char *kdialog[] = { prog, "--title", title, "--getopenfilename", start, NULL };
	char **argv = on_path("zenity", prog, sizeof prog) ? zenity : on_path("kdialog", prog, sizeof prog) ? kdialog : NULL;
	int fds[2];
	if (!argv || pipe(fds))
		return NULL;
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	struct chooser *c = chooser_new(wake, arg, NULL);
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);    /* (GTK's and KDE's chatter) */
	pid_t pid = -1;
	int rc = c ? posix_spawn(&pid, prog, &fa, NULL, argv, environ) : -1;
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	pthread_t t;
	if (rc == 0) {
		c->pid = pid;
		c->out = fds[0];
	}
	if (rc != 0 || pthread_create(&t, NULL, watch, c) != 0) {
		if (rc == 0) {
			kill(pid, SIGTERM);
			waitpid(pid, NULL, 0);
		}
		close(fds[0]);
		if (c)
			chooser_free(c);
		return NULL;
	}
	pthread_detach(t);
	return c;
}

#ifndef __APPLE__
struct chooser *chooser_open(const char *message, void (*wake)(void *), void *arg)
{
	return chooser_spawn(message, wake, arg);
}
#endif
