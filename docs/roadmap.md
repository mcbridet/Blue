# Roadmap

What's next for Blue. The work done so far, task by task, is in
[plan.md](plan.md).

## Next steps

1. More JIT speed: what's left of a boot's host time outside generated
   code is mostly the kernel's ([plan.md](plan.md#findings), Findings, "Where a boot's host
   time goes" and what followed it): host calls, the disk and display
   drivers (5-10%); Multiprocessing Services' calls; kernel calls made in
   place, now 17 ns each with the context switch's copies vectorized; and
   bringing devices up to date, which is guest-visible and stays where it
   is (the exits it cost kernel calls are gone; for quick system calls,
   answering in place saved too little to keep). In the JIT: code runs in
   the interpreter until it's hot (3-7%; translating sooner costs as much
   as it saves) and translating takes 1-4%. On x86-64 the front end still
   limits a Zen 5 (about half its dispatch slots get nothing), now mostly
   for the size of the code ([x86-jit-plan.md](x86-jit-plan.md)).
2. A launcher: a settings window for the disks, CDs, RAM, ROM and shared
   folder, remembered between launches, for Blue.app and the AppImages
   opened from the desktop (which for now show a window saying how to start
   Blue from the command line).
3. Later: host ports forwarded to the guest (so it can serve); the shared
   folder written back when Mac OS ejects the volume and offered again as
   new media, so that changes cross during a run; Glide
   ([glide-plan.md](glide-plan.md)); and, when a program needs them, what OpenGL
   leaves out (OpenGLUtility's tessellators and NURBS, antialiased
   polygons, auxiliary buffers, color index: [opengl-plan.md](opengl-plan.md)).

[future.md](future.md) has ideas not planned yet: a modem, a serial port, infrared, 3D
sound, sound input, AltiVec, printing, game controllers, and more CD
formats.
