#include "env.h"

#include <stdio.h>
#include <string.h>

extern char **environ;

#define OLD_PREFIX "SYSHM_"
#define NEW_PREFIX "BLUE_"

void env_renamed(const char *program)
{
	char line[1024] = "";
	size_t at = 0;
	for (char **e = environ; e && *e; e++) {
		if (strncmp(*e, OLD_PREFIX, strlen(OLD_PREFIX)) != 0)
			continue;
		const char *name = *e + strlen(OLD_PREFIX);
		int len = (int)strcspn(name, "=");
		int n = snprintf(line + at, sizeof line - at, "%s" OLD_PREFIX "%.*s is now " NEW_PREFIX "%.*s",
				 at ? ", " : "", len, name, len, name);
		if (n < 0 || (size_t)n >= sizeof line - at)
			break;
		at += (size_t)n;
	}
	if (at)
		fprintf(stderr, "%s: %s (the old names are ignored)\n", program, line);
}
