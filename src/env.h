/*
 * Blue's settings in the environment are named BLUE_* (BLUE_GPU,
 * BLUE_LOG_MP, ...). Until November 2026 they were SYSHM_*, which are no
 * longer read: env_renamed() says which of those are set, and what they're
 * called now, in one line on stderr.
 */
#ifndef BLUE_ENV_H
#define BLUE_ENV_H

void env_renamed(const char *program);

#endif
