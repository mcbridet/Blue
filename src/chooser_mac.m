/* The host's file chooser on macOS (see chooser.h): an NSOpenPanel, shown without waiting for it */
#import <AppKit/AppKit.h>

#include <stdlib.h>
#include <string.h>

#include "chooser.h"

struct chooser *chooser_open(const char *message, void (*wake)(void *), void *arg)
{
	if (![NSThread isMainThread] || !NSApp)
		return NULL;
	NSOpenPanel *panel = [NSOpenPanel openPanel];
	struct chooser *c = panel ? chooser_new(wake, arg, (__bridge void *)panel) : NULL;
	if (!c)
		return NULL;
	panel.canChooseFiles = YES;
	panel.canChooseDirectories = NO;
	panel.allowsMultipleSelection = NO;
	panel.message = [NSString stringWithUTF8String:message] ?: @"";
	panel.prompt = @"Insert";
	/* (the window's thread goes on; SDL's event loop, which it runs, runs the panel's too) */
	[panel beginWithCompletionHandler:^(NSModalResponse response) {
		NSURL *url = response == NSModalResponseOK ? panel.URL : nil;
		char *path = url.isFileURL ? strdup(url.fileSystemRepresentation) : NULL;
		chooser_done(c, path ? CHOOSER_CHOSEN : CHOOSER_CANCELLED, path);
	}];
	return c;
}

void chooser_cancel_panel(void *panel)
{
	[(__bridge NSOpenPanel *)panel cancel:nil];
}
