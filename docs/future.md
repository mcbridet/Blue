# Future features

Ideas worth doing some day, beyond the [roadmap](roadmap.md). Each says
what the real iMac had, what it would be good for under Blue, and what it
would take.

## Printing

- **The iMac's:** no printer port. Printers were USB ones with their own
  drivers, or network printers: LaserWriters over AppleTalk, or any
  PostScript printer by LPR over TCP/IP (LaserWriter 8's Desktop Printer
  Utility makes an LPR desktop printer).
- **What for:** printing from Mac programs to a PDF on the host, or to the
  host's printer.
- **How:** Blue's network gateway (10.0.2.2) answers LPR (port 515);
  Mac OS's LaserWriter 8 sends it PostScript, and Blue writes the job out
  as a PDF in a folder (an option, the Desktop by default) with
  Ghostscript's pdfwrite (found at run time; recent macOS no longer
  converts PostScript itself), or hands it to the host's printing system
  (`lp`: CUPS, on macOS and Linux). Without Ghostscript the PostScript file
  is kept and Blue says so. The guest tools carry a PPD for a "Blue PDF
  Printer" (paper sizes, colour), and the install prompt could set up the
  desktop printer. Programs that draw with QuickDraw print too: LaserWriter
  8 turns QuickDraw into PostScript. The NAT has to accept connections to
  the gateway's own address for this. Real-time runs only, as for all
  networking.
- **Alternatives:** a LaserWriter on AppleTalk (PAP) at the gateway, which
  needs AppleTalk to get past the NAT (only IP does today), or a printer
  driver of Blue's own in the Chooser (far more work).
- **Size:** moderate: an LPD listener in the NAT, the job handling, the PPD.
