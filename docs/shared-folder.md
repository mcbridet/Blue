# The shared folder

How `--share DIR` works: a folder of yours shown to Mac OS as a disk, what
Mac OS changes on it written back when it puts the disk away or shuts
down, and the disk offered again with the folder as it is then. The short
version is in the [README](../README.md#the-shared-folder).

`--share DIR` shows a host folder to Mac OS as a volume of its own, named
after the folder, with the files and folders in it: the way to move files
between the host and Mac OS, such as a program downloaded on the host to
install. It's an ordinary HFS volume, so everything works on it (the
Finder, installers, programs run from it), and what Mac OS changes on it
goes back to the folder when Mac OS puts the volume away or shuts down.

```
build/blue boot --disk hd.img --share ~/Mac --nvram blue.nvram
```

The volume (`src/share.c`) is made when Blue starts: its catalog and the
rest of its metadata in memory (`src/hfs.c` lays volumes out), its files'
contents read from the host's files only as Mac OS reads them. It has half
as much free space as there is in it, at least 1 GB but under 2 GB (older
installers count free space in 31 bits). It is attached after the other
drives, as one of the four, and is never the startup disk (no boot blocks,
no System Folder). Names become Mac OS Roman, with `/` for `:` as macOS
shows them; one longer than 31 characters, or the same as another's to
Mac OS (which ignores case), is shortened with the item's ID, as Mac OS 9
does (`A very long file name#1A.txt`). Dot files, symbolic links to
folders and files over 2 GB (HFS's limit) are left out, and Blue says so.
Dates are the host's, but none later than the guest's clock as the run
starts (`--fixed-time`'s, else the host's), and none created after it was
modified (as a file macOS copied or unpacked is: it's created when it was
modified): Norton Utilities takes either for damage, a volume's creation
date in the future for "damage to the Master Directory Block (Error
16,005)". The volume was created when the folder was (on Linux, when it
last changed), so Mac OS knows it from run to run.
Resource forks and Finder info (type, creator, flags) are the host's: on
macOS the file's own (`..namedfork/rsrc`, `com.apple.FinderInfo`), and
otherwise an AppleDouble file, `._name` beside it or in a `__MACOSX` folder
(as zip archives keep them). A file without Finder info gets a type and
creator from its extension (`.sit`, `.hqx`, `.bin`, `.zip` for StuffIt
Expander; `.txt`; `.jpg`, `.gif`, `.png` for PictureViewer; `.pdf`; `.img`,
`.dmg`, `.toast`, `.dsk` for Disk Copy; `.mov`, `.mp3`, `.aiff`, `.html`
and a few more), else SimpleText's if it's plain text, else `????`.
Archives in the StuffIt 5 format (from 1998 on; `lsar` calls them "StuffIt
5") need StuffIt Expander 5 or later; Mac OS 8.5 and 8.6 come with 4.5,
which quits without a word on them. Either install Expander 5.5 (as a
`.hqx` or `.bin`, which 4.5 opens), or expand on the host with The
Unarchiver's `unar` (Homebrew's `unar`), which keeps resource forks and
Finder info on macOS, so they reach Mac OS through the share.

What Mac OS writes goes to a temporary file (in `$TMPDIR`, gone when
Blue exits), never to the folder while Mac OS has the volume. Once Mac OS
has put the volume away (Put Away, Eject or the Trash during the run,
below; Shut Down at the end), Blue writes back what changed: new,
changed, renamed, moved and deleted files and folders,
with their resource forks and Finder info as the host keeps them (on
macOS the file's own; otherwise an AppleDouble `._name` file, only for a
file with a resource fork or Finder info its name and contents wouldn't
give it) and their dates. Nothing of the host's is destroyed: a file or
folder Mac OS deleted (emptying the Trash or not) or replaced is moved to
`DIR/.blue-recovery/<date and time>/`, under the path it had, and a file
that changed on the host as well during the run is kept, with Mac OS's
version beside it as `name (Mac OS).ext`. What Mac OS and its extensions
keep on a volume for themselves (the desktop database, the Trash, invisible
files at its top) isn't written back. Blue lists what it did:

```
share: writing back what Mac OS changed on "Shared" to /Users/me/Shared
share:   changed Read Me.txt (the host's was put in .blue-recovery/2026-11-15 00.13.05)
share:   new untitled folder
share: 1 new, 1 changed, 0 moved or renamed, 0 deleted; the host's old ones are in /Users/me/Shared/.blue-recovery/2026-11-15 00.13.05
```

If the run ends without Mac OS having put the volume away (Turn off, in
Blue's menu or when it asks on quitting, rather than Mac OS shutting down),
nothing is written back, as the volume may be half-written; if anything
of the user's had changed on it, it is saved as a disk image in the
recovery folder instead (attach it with `--disk` to get at what's on it;
Mac OS may check it first, as after a crash). Mac OS marks the volume in use
in its MDB as soon as it mounts it (its first write) and put away when it
unmounts it; that's how Blue tells. A restart keeps the volume as Mac OS
left it. `BLUE_LOG_SHARE=1` logs Mac OS's writes. Text files keep their
line endings (a host's LF shows as boxes in SimpleText). Folders' Finder
info (window positions, custom icons) and the Locked checkbox aren't
written back.

## Put away and offered again

In a window, in real time, and in input scripts that use it (`share`
commands, below), the volume's drive is a removable one, as the CD-ROM
drives are where discs can be swapped: Mac OS can eject its disk. (An
ejectable drive changes what Mac OS does all along, so other
instruction-time runs keep it fixed, as before, the same instruction for
instruction; there changes cross only at the end of the run.) The drive
(`disk_open_removable()` in `src/disk.c`) is a disk drive, writable,
whose disk stays in at a restart and at shutdown (DriverGestalt `ejec`, as
the CD-ROM and floppy drives answer), with icons of its own (a disk with
a folder on it), which Mac OS 8.5's Finder needs (see
[findings.md](findings.md), "Ejecting the shared folder's volume").

When Mac OS ejects it (Put Away, dragging it to the Trash, Special >
Eject, Command-E, a program's UnmountVol and Eject), Blue looks at what
Mac OS left: if it has no VCB for the volume (not even off line) and the
volume's MDB says it was put away, what Mac OS changed is written back
there and then (`share_write_back()`), with the same rules, recovery
folder and summary as at the end of the run; the volume is made again
from the folder as it is now (`share_rebuild()`: what the host added,
changed or deleted meanwhile is on it, its dates no later than the
guest's clock now, and what Mac OS wrote on the old one gone), and it
goes in the drive again as a new disk, which Mac OS sees a second later
and mounts, as a disk put in:

```
share: Mac OS put "Shared" away
share: writing back what Mac OS changed on "Shared" to /Users/me/Shared
share:   changed Read Me.txt (the host's was put in .blue-recovery/2026-11-17 00.00.16)
share: 0 new, 1 changed, 0 moved or renamed, 0 deleted; the host's old ones are in /Users/me/Shared/.blue-recovery/2026-11-17 00.00.16
share: /Users/me/Shared as "Shared" again: 4 files, 1 folders, 1024 MB free
```

If Mac OS ejected the disk without putting the volume away (Eject leaves
it off line, its VCB kept, and Mac OS asks for the disk when it's needed
again), nothing is written back, and the same disk goes back in as Mac OS
left it. A volume written back during the run isn't written back again:
what's written back at the end (or the next time it's put away) is only
what Mac OS changed on the volume offered since. If the folder can't be
made into a volume again (it's gone), the drive stays empty, and Refresh
tries again.

**Refresh** in Blue's menu (F12), `Refresh "Shared"`, brings the host's
changes across without the user putting the volume away: Blue has Mac OS
put it away (`src/putaway.c`), then all goes as above. Code of Blue's runs
in Mac OS at its next idle call, as the guest tools' installer does
([guest-tools.md](guest-tools.md#offering-and-installing-them); the
bootstrap is `install_divert()`), and does what a program putting a disk
away does: `UnmountVol` and `Eject` of the volume's drive number. Mac OS
refuses `UnmountVol` while files on the volume are open (`fBsyErr`), and
the menu opens again to say so. Blue disables the button while it sees
files open there, its tooltip saying so: it counts the files open on the
volume in Mac OS's FCB table, but for the volume's own (its B-trees, the
desktop database);
Mac OS 9 keeps only a stand-in table, of the volumes' VCBs alone, where
the volume's own four are counted out. When the volume isn't mounted
(not yet, or put away without being ejected), Refresh makes it again at
once. The menu also says when the folder has changed on the host since
the volume was made: when it opens, Blue looks at each item's host file
(its inode, size and modification time) and at each folder whose
modification time changed, at whether what's in it that Mac OS would see
did (a `.DS_Store` doesn't count). Nothing is refreshed by itself.

Input scripts have `share eject` (Mac OS is asked to put the volume away
as Refresh asks it, whatever it has open: it refuses while files are
open), `share refresh` (the menu's Refresh, which doesn't ask then) and,
for tests, `share file NAME TEXT`, which writes
TEXT (the rest of the line, with `\n` for a newline, as for `clipboard`)
to NAME in the folder, as if the host had, at that instruction count; any
`share` command makes the drive removable. See
[debugging.md](debugging.md#input-scripts).
