/*
 * Blue's menu bar on macOS (menubar_mac.m), in place of the one SDL makes
 * when it starts (SDL makes its own whenever it makes the application, so
 * it's replaced once SDL is up), whose Command-key shortcuts acted on Blue
 * as well as reaching Mac OS: Command-Q quit at once, Command-W closed the
 * window, Command-H hid Blue, Command-M minimised it. Blue's has only the
 * application menu: About Blue, Hide Blue and Quit Blue (which asks first,
 * as closing the window does), without shortcuts, so that every Command
 * key is Mac OS's. On the window's thread, once SDL's video is up.
 */
#ifndef BLUE_MENUBAR_H
#define BLUE_MENUBAR_H

#include <stdbool.h>

void menubar_install(void);

/* Command-H and Command-Q for Hide Blue and Quit Blue (the window shown without Mac OS), or none */
void menubar_shortcuts(bool on);

#endif
