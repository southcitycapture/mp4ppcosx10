# Simulated machines for `--fake-machine`

Each file is `key=value` lines overriding one probe of
`src/platform/machine.c` (the keys are listed in `--help`).  Only the keys
named are overridden; everything else is the real machine's.  The extension
lists are reconstructions of Apple's 10.4-era drivers for cards the project
does not own -- **the real answer on a real machine is `--machinecheck`**, and
these files exist to exercise the verdict logic, not to stand in for it.

| file | machine | what it argues |
|---|---|---|
| `g4-500-radeon7500.txt` | a single 500 MHz G4 (PowerMac3,4 "Digital Audio") with a 32 MB Radeon 7500, 10.4.11 | the slow single core, a small card, a 3-unit combiner |
| `imac-g4-geforce4mx.txt` | an 800 MHz iMac G4 17" (PowerMac4,5) with its 32 MB GeForce4 MX, 10.4.11 | an NVIDIA list: no ATI combine3, two texture units |
| `g4-radeon9000-32mb.txt` | the project's own G4 with a 32 MB Radeon 9000 (the extension list is `g4-glinfo.log`'s) | the VRAM rule alone |
