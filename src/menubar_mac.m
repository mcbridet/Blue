/* Blue's menu bar on macOS (see menubar.h) */
#import <AppKit/AppKit.h>

#include "menubar.h"

static NSMenuItem *hide_item, *quit_item;

void menubar_install(void)
{
	if (![NSThread isMainThread] || !NSApp)
		return;
	NSMenu *app = [[NSMenu alloc] initWithTitle:@"Blue"];
	[app addItemWithTitle:@"About Blue" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
	[app addItem:[NSMenuItem separatorItem]];
	hide_item = [app addItemWithTitle:@"Hide Blue" action:@selector(hide:) keyEquivalent:@""];
	[app addItem:[NSMenuItem separatorItem]];
	/* (SDL's application has terminate: send SDL_QUIT, as the Dock's Quit and logging out do) */
	quit_item = [app addItemWithTitle:@"Quit Blue" action:@selector(terminate:) keyEquivalent:@""];
	NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Blue" action:nil keyEquivalent:@""];
	item.submenu = app;
	NSMenu *bar = [[NSMenu alloc] initWithTitle:@""];
	[bar addItem:item];
	NSApp.mainMenu = bar;
	/* (SDL's Window menu, with Close and Minimize, is gone: AppKit lists no windows in it) */
	NSApp.windowsMenu = nil;
}

void menubar_shortcuts(bool on)
{
	if (![NSThread isMainThread])
		return;
	hide_item.keyEquivalent = on ? @"h" : @"";
	quit_item.keyEquivalent = on ? @"q" : @"";
}
