# Future features

Ideas worth doing some day, beyond the [roadmap](roadmap.md). Each says
what the real iMac had, what it would be good for under Blue, and what it
would take.

## Sound input (microphone and line in)

- **The iMac's:** a built-in microphone above the screen and a stereo
  sound-in jack, both into the sound chip's input, chosen in Monitors &
  Sound (Sound control panel in Mac OS 9).
- **What for:** recording in Mac OS's own programs (SimpleSound, alert
  sounds, sound editors), PlainTalk speech recognition and Speakable Items
  with the host's microphone, and games or programs that listen.
- **Where Blue is:** the codec's input DMA channel (9) runs at the right
  pace but records silence, and the sound node declares output hardware
  only (`hardware-types` 2), because Mac OS 9's AWACS input component
  ('sind' 'awgc') has a bug in its close routine that corrupts the system
  heap at startup ([bring-up.md](bring-up.md#mac-os-9), Mac OS 9). Declaring input again brings that
  back, so that comes first. Options: declare input only for systems
  without the bug (8.5 and 8.6 use another sound stack; check which have
  it), make the bug harmless (a RAM patch of the component, with the
  injection the guest-tools installer brings), or give Mac OS 9 its input
  another way, as an emulated USB audio device on Blue's USB controller
  (like Griffin's iMic, which iMac owners used), where Mac OS 9's USB audio
  support handles it; whether 8.5 and 8.6 have USB audio support is to be
  checked.
- **How (host side):** capture with SDL2 (loaded at run time, as for
  output) from the host's default input, or one chosen with an option, in
  real-time runs only. Captured frames go into the input DMA buffers at the
  guest's rate (rate-matched as output is), with the codec's input source
  and gain bits applied. Instruction-time runs keep recording silence, or
  read a WAV file given with an option, so tests stay reproducible.
- **Privacy:** the host's microphone opens only while Mac OS is actually
  recording (input DMA running) and closes after; the status bar shows a
  light while it's open; on macOS the system asks the user's permission the
  first time.
- **Size:** moderate: the host capture and DMA side are small; the Mac OS 9
  input component is the uncertain part.
