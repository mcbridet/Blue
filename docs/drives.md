# Disks, CDs and floppies

How Blue's drives work: the disk driver, the removable drives (floppy
and CD-ROM), CD images with audio, compressed images, and floppy images. How to use them is
in the [README](../README.md#disks-cds-and-floppies); the shared folder,
which is a drive too, is in [shared-folder.md](shared-folder.md).

## The disk driver

The disk driver (`src/disk.c`) takes host files as drives (`--disk`, writable; `--cdrom`, read-only;
the 8.5 CD's System refuses to start from a writable drive, as from a
copy of the CD), and a host folder (`--share`; see [shared-folder.md](shared-folder.md)),
whose blocks come from `src/share.c` rather than a file; and a floppy
drive (`--floppy`): the floppy and CD-ROM drives' discs come and go
(see [Removable drives](#removable-drives)). CD-ROM drives answer as Apple's CD-ROM driver
(`src/cdrom.c`: the table of contents, audio, icons), their discs
plain images, images with tracks (`src/cdimage.c`, `src/cdformats.c`) or
compressed Mac images (`src/dmg.c`); see [CD images](#cd-images) and
[Compressed images](#compressed-images).
An Apple partition map gives one drive per HFS partition (a GUID
partition table, as images made on today's Macs have, one per HFS+
partition); otherwise the file is one volume. (When there's a map entry at offset 512, entries are
512 bytes apart and count 512-byte blocks even if the driver descriptor
gives larger ones, as on CDs with 2048-byte blocks.) A timer posts
disk-inserted events until each volume has been mounted (the driver
loads before events are enabled). The
drives decline to hold virtual memory backing store (DriverGestalt
`vmop`), so Mac OS boots with VM off. The kernel doesn't page; give the
guest more RAM with `--ram` instead.
Transfers take guest time, at 20 MB/s on disks and 4 MB/s on CDs (at
most a tick each, so that a big one doesn't jump past what interrupts
keep going, such as sound). Without it, a program's launch or a file
copy took no guest time at all, and Mac OS counts a process's processor
time in whole ticks: the Mac OS 9 installer waits until its engine has
used a tick, which on a fast guest it could take forever to do. In real
time, guest time runs ahead of the host clock by what transfers took,
and catches up while the guest idles (running at 3/4 speed, unless sound
plays). While sound plays, though, guest time ahead would be sound
played ahead, which the host can't play: there a transfer takes host
time, all of it, as a real drive's DMA does while the processor goes
on. The guest waits in the driver's call until the transfer is over,
taking interrupts meanwhile, so the sound keeps the host clock's pace (a
movie streaming from disk used to run guest time 1.1 to 2.3 times as
fast, its sound dropped in pieces; [plan.md](plan.md#findings), Findings).

## Removable drives

The floppy drive (`--floppy`) and the CD-ROM drives (`--cdrom`) take disc
images that come and go while Mac OS runs, mainly to install software that
came on floppies or on several CDs. Mac OS ejects a disc as on any Mac: Put
Away or dragging it to the Trash, Eject in the Finder's Special menu, an
installer's own eject, an Initialize dialog's Eject. The drive is then
empty, and Blue says so (`cdrom: Mac OS ejected ut.iso`). To put an image
in, drop it on the window: one of a floppy's size (2880K or less) goes in
the floppy drive, any other in a CD-ROM drive, the first that's empty (else
one whose disc Mac OS isn't using). Mac OS learns of a disc going in as
from a real drive, by a disk-inserted event, so whatever application is in
front and waiting for one (an installer, Disk Copy) gets it, and mounts it.

In a window or real time there's always a floppy drive, and a CD-ROM drive
unless one was given: Blue adds an empty one of each (as `--floppy-drive`
and `--cdrom-drive` do), so a disc can go in whatever Mac OS started with.
The CD-ROM drive is one of the four disks: with four already, there's none
(Blue says so, `drives: no CD-ROM drive: all 4 drives are taken`).
Instruction-time runs without a window (`--headless`) have only the
drives given, as before, so they stay reproducible.

```
build/blue boot --disk hd.img
build/blue boot --disk hd.img --cdrom "Game CD 1.iso"
build/blue boot --disk hd.img --floppy "Install 1.image" --floppy "Install 2.image"
```

The window's menu (F12) shows what's in each drive, with Eject and
Insert... buttons. Insert... opens the host's file chooser (on macOS the
system's Open panel; on Linux zenity's or kdialog's, if one is installed),
and the image chosen goes in that drive as a dropped one would, with the
same checks (without a chooser, the menu says to drop an image on the
window). Neither the window nor Mac OS waits for the chooser. Blue won't
take a disc out that Mac OS has mounted (it's using it: Eject is disabled,
and the menu says to put it away in Mac OS); one that isn't mounted
(unreadable, say, or just put in) it can, and Insert... puts another in its
place. If an image can't go in (no empty drive, a drive Mac OS is using, not
an image Blue takes), the menu opens to say why.

`--floppy IMAGE` puts a floppy disk image in the floppy drive; given
several times, for a set of disks, the first is in the drive at startup
and the others go in, in turn, as Mac OS ejects the one in (a second
later, as from a quick hand): a multi-disk installer that ejects disk 1 and
asks for disk 2 gets it with nothing to do, and the menu lists what's still
to come. Dropping an image puts that one in instead (when the drive is
free), and the rest follow as before. `--floppy-drive` gives a floppy drive
with nothing in it, `--cdrom-drive` an empty CD-ROM drive (one of the four
disks): in a window or real time only for a second empty CD-ROM drive, say,
as there's one of each anyway. Input scripts have `floppy insert FILE` and
`cdrom insert FILE` (the rest of the line), `floppy eject` and `cdrom
eject` (the first CD-ROM drive whose disc Mac OS isn't using), which follow
the same rules.

CD-ROM drives answer as CD-ROM drives (see [CDs: audio and cue sheets](#cds-audio-and-cue-sheets)
below), and the swappable ones say their disc is ejectable. As in an iMac,
a disc stays in its drive through a restart (Special > Restart, the guest
tools' Install and Restart, the menu's Restart) and when Mac OS shuts
down, and is mounted again after the restart; so does a floppy (see
[Floppies](#floppies)), and the next of several `--floppy` images doesn't go in then.
A CD-ROM drive gives
Mac OS a drive for each HFS partition of the disc in it at startup (one if
it's empty); a disc put in later with more shows its first ones, and a
disc comes out once none of its volumes is in use. A CD put in while Mac OS runs is never
the startup disk (it's shown to Mac OS once applications run); one in at
startup may be, as before, and Mac OS won't eject the startup disk ("The
startup disk cannot be removed from the desktop"). CD images are raw (an
`.iso`, `.toast`, `.cdr` or `.img`, with or without a partition map),
compressed (`.dmg`, Disk Copy 6) or have tracks (cue sheets, `.nrg`,
`.mds`, `.ccd`, `.chd`: see [CD images](#cd-images)); archives and other
kinds are refused, saying what they are, and so is a `.bin` of raw sectors
given without its `.cue` and with none beside it (when it starts with a
data track; one that starts with audio can't be told from a plain image). Ejectable drives change what Mac OS does all along (it looks at
the drives' flags as it runs), so CD-ROM drives are ejectable only where
discs can be swapped: in a window or real time, with `--cdrom-drive`, or
with an input script that swaps CDs (`cdrom` commands); other
instruction-time runs keep them fixed, as before, the same instruction for
instruction.

## CDs: audio and cue sheets

A CD-ROM drive (every one, swappable or not) answers Mac OS as Apple's
CD-ROM driver does on a real Mac (`src/cdrom.c`, after Apple's Technical
Note DV22): its driver is named `.AppleCD`, DriverGestalt says `cdrm` (and
`ejec`, that a restart or shutdown leaves the disc in), and
it has the calls that AppleCD Audio Player, the Control Strip's CD module,
Audio CD Access, QuickTime and games make: the table of contents
(ReadTOC), where the drive is (AudioStatus, the Q subchannel), playing
(AudioPlay, AudioTrackSearch, AudioPause, AudioStop, AudioScan), the
drive's volume and channels (AudioControl, ReadAudioVolume), the track
play lists the driver plays itself (AppleCD Audio Player's shuffle and
programs, with repeat), reading audio digitally (ReadAudio, with the Q or
all of the subchannel). The Finder shows a CD and a CD-ROM drive icon (a
disc's own volume icon, if it has one, goes before them), and Get Info says
"Blue CD-ROM drive".

Images with audio come as cue sheets (`.cue`, with `.bin` files, as most
CD rips are), or in the other layouts of [CD images](#cd-images): tracks in
raw 2352-byte sectors (audio; mode 1 or mode 2 data), in 2048- or 2336-byte
data sectors, or in audio files (WAVE or AIFF at 44.1 kHz, 16-bit stereo;
FLAC, MP3 or Ogg Vorbis at 44.1 kHz, decoded as they're read); `BINARY` and
`MOTOROLA` files, one for the disc or one a track; `INDEX 00` pregaps in the
file and `PREGAP`/`POSTGAP` silence that isn't; several sessions (`REM
SESSION`, as on CD-Extra discs); `CATALOG` and `ISRC`. The disc's own
addresses are worked out as a drive has them (track 1 at 00:02:00). `blue
disk toc FILE` lists what the drive will say:

```
$ build/blue disk toc "Quake [1996].cue"
Quake [1996].cue: a cue sheet, 11 tracks, 1 session, lead-out at 61:47:11 (LBA 277886)
   1  mode 1  00:02:00  LBA      0  02:47:20  (Mac OS sees its data)
   2  audio   02:51:20  LBA  12695  05:08:38  (pregap 00:02:00)
   3  audio   07:59:65  LBA  35840  02:25:60  (pregap 00:00:07)
   ...
```

Mac OS mounts the data track (the first one): an HFS volume, with or
without a partition map, or ISO 9660 with ISO 9660 File Access (in an
installed system: the PC Quake CD shows up as QUAKE106). A disc of audio
alone reads as blank data, and Foreign File Access with Audio CD Access
mounts it as "Audio CD 1", its tracks as documents (on 8.5; AppleCD Audio
Player opens them, QuickTime's MoviePlayer converts them to AIFF files,
reading them digitally).

The drive plays audio as the iMac's did: into the sound chip's analog input,
which Mac OS sets to play the CD through to the outputs (8.5 selects the CD
and turns loop-through on; with no input selected, as Mac OS 9 leaves it
here, it plays through too). So CD audio is mixed with Mac OS's own sound
and follows its volume and mute (Monitors & Sound), as on the real machine,
and the drive's own volume (AudioControl) comes before. It's paced by guest
time: where the drive is follows from where and when play started, so
instruction-time runs are reproducible, and `BLUE_SOUND_WAV` records it.
To play a music CD's or a game's soundtrack, start Mac OS with the cue
sheet, or drop it on the window:

```
build/blue boot --disk hd.img --cdrom "Quake [1996].cue"    # then AppleCD Audio Player, in the Apple menu
```

On an installed 8.5, the Control Strip's CD module plays a disc that's in at
startup by itself (its AutoPlay); for one put in later it keeps the table of
contents it read at startup (none) and doesn't, while AppleCD Audio Player
plays either. AppleCD Audio Player numbers a mixed disc's audio tracks from
1 (on the Quake CD, its Track 1 is the disc's track 2). AppleCD Player (in
the 8.5 CD's CD Extras) plays an audio CD as soon as it opens, from the
first CD-ROM drive (it opens `.AppleCD`, the first driver of that name).

## CD images

Each kind of image is told by its contents (`cdimage_detect()`), not its
name: CHD's and Alcohol's magic numbers, Nero's trailer, CloneCD's and a
cue sheet's text (a `.cue` by its name too), UDIF's `koly` trailer and
NDIF's resource fork (`src/dmg.c`); anything else is a plain image. Given
the data of an image whose table of contents is in a file beside it (a
`.bin` of a cue sheet, an Alcohol `.mdf`, a CloneCD `.img` or `.sub`, a
cdrdao `.toc` with a `.cue`), Blue opens that file instead, so dropping
either works. Each layout gives the same table of contents, sectors and
sound as the cue sheet of the same disc, and `blue disk toc` says what
kind it read:

- **Cue sheets** (`src/cdimage.c`): as above. An audio file's kind is
  what's in it, whatever its `FILE` line says: WAVE and AIFF as they are;
  FLAC, MP3 and Ogg Vorbis decoded as they're read (`src/audiodec.c`:
  dr_flac, dr_mp3 with a seek table of a point a second, stb_vorbis),
  reading on from where the last read stopped, seeking only when the drive
  jumps; mono is played on both channels. Their lengths are what the files
  say (an MP3's LAME header trims the encoder's delay and padding), so a
  track of a lossy rip may be a sector longer than the CD's.
- **Nero** (`.nrg`, `src/cdformats.c`): the chunks the trailer points to
  (`NER5`, 64-bit; `NERO`, 32-bit): disc-at-once sessions (`CUEX`/`CUES`
  cue points for the addresses, `DAOX`/`DAOI` for where each track's
  pregap, start and end are in the file and its kind of sector, with or
  without subchannel), and track-at-once ones (`ETN2`/`ETNF`). Audio is
  little-endian, as in a `.bin`.
- **Alcohol 120%** (`.mds`, version 1, with its `.mdf`, little-endian
  structures): each session's track entries (start, file offset, sector
  size, with or without the 96 bytes of subchannel after each), their
  extra blocks (pregap and length) and the lead-out's entry; the file each
  names (`*.mdf`: the `.mds`'s name). A DVD's image or the later version
  (`.mdx`) is refused.
- **CloneCD** (`.ccd`, with its `.img` and `.sub`): the TOC entries
  (each track's start, its control bits; each session's lead-out) and the
  `[TRACK]` sections' indexes; the `.img` holds every track from its
  pregap on, in order, 2352 bytes a sector. The `.sub` isn't read.
- **CHD** (`.chd`, MAME's, through libchdr: v5's `cdlz`, `cdzl`, `cdfl`
  and `cdzs` codecs, and the older versions): the tracks' metadata
  (`CHT2`, or `CHTR`), each track's frames padded to a multiple of four in
  the file, a pregap stored when its type starts with `V`; audio is
  big-endian there. Four hunks are kept. A CHD that needs its parent, a
  GD-ROM's, or a hard disk's is refused.

Checked with the PC Quake CD's cue/bin (copies): made into FLAC, MP3 and Ogg
Vorbis cue sheets (ffmpeg), Nero, Alcohol (with and without subchannel) and
CloneCD images (a script of the formats' layouts; MAME's chdman reads the
Nero one), and a CHD (chdman): every sector of every lossless one the same
as the cue sheet's, the whole disc read in 1.5 to 2.6 s; the same with an
HFS data track in place of its ISO 9660 one, in a second CD-ROM drive
beside the 8.5 CD as the startup disk: Mac OS mounts it, AppleCD Player
plays track 2, and what the sound chip plays is the same, sample for
sample, from each lossless image (and, scaled by the volume, track 2's
samples), 25 dB and 18 dB from it for the MP3 and Ogg Vorbis ones (their
encoders' loss).

## Compressed images

UDIF (`.dmg`) and Disk Copy 6's NDIF (`.img`) images are read as they're
compressed, a chunk at a time (`src/dmg.c`, with the last four chunks
unpacked kept; hdiutil's chunks are 1 MB at most), read-only: in a CD-ROM
drive (a disc of one data track), in the floppy drive if unpacked they're
of a floppy's size, or as a `--disk`, which is then locked. A dropped image
goes to the floppy drive or a CD-ROM drive by its size unpacked.

- **UDIF:** the `koly` trailer gives the block table, in an XML property
  list (`blkx`, each partition a `mish` table in base64) or, in early
  images (Disk Copy 6.3 to Mac OS X 10.1), in `blkx` resources of a
  resource fork appended to the data fork, each partition's chunks counted
  from its own data offset (hdiutil and 7-Zip call those corrupt). Chunks:
  raw, zeros, ADC, zlib (miniz), bzip2, LZFSE and xz (LZMA2, the LZMA
  SDK's decoder; Apple's chunks are xz streams of LZMA2 alone). A UDIF in
  MacBinary is read too. Encrypted and segmented images are refused.
- **NDIF:** the `bcem` resource of its resource fork (versions 10 to 12:
  the image's blocks, then chunks of raw, zero and ADC data in the data
  fork). The fork comes from MacBinary, AppleSingle, an AppleDouble file
  beside it (`._NAME`, or `.AppleDouble/NAME`) or macOS's named fork; without
  it the data fork alone is a raw image (which a read-only NDIF's is).
  KenCode chunks are refused.

Checked: hdiutil's UDZO, UDBZ, ULFO, ULMO and UDCO images of an HFS volume
unpacked to what hdiutil's own conversion makes; `Unreal Tournament.dmg`
(a UDIF of raw and zero chunks) to the same bytes as the `.iso` made from
it; an early UDIF of an Apple partition map's disk (ADC chunks, three
partitions with their own data offsets, which hdiutil calls corrupt), an
NDIF with its AppleDouble file, and each of these in a second CD-ROM drive
beside the 8.5 CD, mounted by Mac OS (Unreal Tournament's volume too); a
400K image in the floppy drive (locked) and an early UDIF as a `--disk`,
mounted.

## Floppies

Images: raw ones of 400K, 720K, 800K, 1440K and 2880K disks (`.img`,
`.dsk`, `.ima`, `.flp`), and Disk Copy 4.2 ones (`.image`: an 84-byte
header, the blocks, then the 400K and 800K disks' tags; also in
MacBinary), whose checksums Blue keeps right as Mac OS writes (they're
worked out again a second after the last write, and when the disk is
ejected or flushed). They're told apart by their contents and size, not
their names. Compressed UDIF (`.dmg`) and Disk Copy 6 (NDIF) images of
those sizes go in read-only, unpacked as they're read (see [Compressed
images](#compressed-images)); DART images and archives (StuffIt, zip,
gzip, BinHex, ...) aren't taken: Blue says what they are, or that their
size is no floppy's. Convert them to a raw or Disk Copy 4.2 image first
(with Disk Copy in Mac OS, or a tool on the host). An
image is read-only (Mac OS shows it locked) if its file can't be written,
or is locked in the Finder; lock an image to keep it as it was, as Mac OS
writes to a writable disk as soon as it mounts it (its volume information,
the desktop database). Mac OS mounts what it can read: HFS, FAT (PC
floppies) with File Exchange, which a full install has (the 8.5 CD's
System doesn't: it offers to initialize them). MFS disks (System 1-6 era,
400K) are refused: Mac OS 8.5 and later can't read them. A blank image (or
one Mac OS can't read) brings up Mac OS's "unreadable, initialize?"
dialog, and Erase Disk works on floppies (Format, Verify, then a new
volume; the format offered is the image's size). `blue disk create
blank.img 1440K "Untitled"` makes a blank formatted floppy (400K, 720K,
800K or 1440K).

The iMac has no floppy controller, so none is emulated: the drive is a
third kind of drive of Blue's own disk driver (`src/disk.c`, images in
`src/floppy.c`, which drive an image goes in `src/drives.c`), at PCI
device 7, much as a USB floppy drive is on a real iMac, and needs nothing
installed in Mac OS. It answers as a floppy drive
(DriverGestalt 'devt' 'flop', a floppy disk's icon and the drive's, in
color, the 'where' of Get Info, the format list) and says its disk is
ejectable. As the CD-ROM drives do, it tells Mac OS to leave the disk in
when it restarts or shuts down (DriverGestalt 'ejec'), and the disk is
mounted again after a restart (checked on 8.5, 8.6, 9.1 and 9.2.2):
Command-E, Put Away and the Trash eject it (and the next of several
`--floppy` images goes in), a restart doesn't. A floppy is never the
startup disk: the drive is empty as far as Mac OS knows until applications
run (see [plan.md](plan.md#findings), Findings), and Blue doesn't look for a ROM file on it.
Transfers take guest time at the disks' rate. The status bar's floppy light
(with a floppy drive only) is dimmed while it's empty. Disk Copy can't
make floppies from images in this drive (it looks for Apple's floppy
drives): put the image in instead, or a copy of it.
