# The shared folder

How `--share DIR` works: a folder of yours shown to Mac OS as a disk, and
what Mac OS changes on it written back when it shuts down. The short
version is in the [README](../README.md#the-shared-folder).

`--share DIR` shows a host folder to Mac OS as a volume of its own, named
after the folder, with the files and folders in it: the way to move files
between the host and Mac OS, such as a program downloaded on the host to
install. It's an ordinary HFS volume, so everything works on it (the
Finder, installers, programs run from it), and what Mac OS changes on it
goes back to the folder when Mac OS shuts down.

```
build/blue boot --disk hd.img --share ~/Mac --window --nvram blue.nvram
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
Blue exits), never to the folder during the run. At the end of the run,
if Mac OS put the volume away (Shut Down, or Put Away), Blue writes back
what changed: new, changed, renamed, moved and deleted files and folders,
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
left it. `BLUE_LOG_SHARE=1` logs Mac OS's writes.

Changes cross only once a run: files added on the host during the run
appear in Mac OS the next time, and changes made in Mac OS reach the host
at the end. Text files keep their line endings (a host's LF shows as boxes
in SimpleText). Folders' Finder info (window positions, custom icons) and
the Locked checkbox aren't written back.
