# AGENT.MD — Orange Pi 3B CD/DVD/BD emulator

This file is the entry point for a new developer or an AI coding agent.
Read this, then `TODO.MD`, then `README.md` and `docs/`.

## Project identity

- **Positioning:** high-capacity / high-performance USB optical emulator.
- **Hardware:** Orange Pi 3B (RK3566), Debian, USB 2.0 device port
  (`fcc00000.usb`), optional SSD1306 OLED + EC11 encoder.
- **Host view:** SCSI MMC optical drive (`PDT=5`), not a USB mass-storage
  stick. Supports CD-ROM, CD-RW, DVD-RW and BD-RE images.
- **Sibling project:** `rp2350-cdrw-emu` — low-cost, plug-and-play,
  USB 1.1, CD-RW only. This project is the performance/large-capacity one.

## Build and test

```bash
make -j4
make test                    # host-side SCSI/media/BOT tests
sudo deploy/install.sh       # install to /opt and restart systemd
systemctl status pi3b-cdrw
journalctl -u pi3b-cdrw -f
```

No Pico SDK is needed. Only gcc/make for the daemon. The daemon uses
FunctionFS + a custom Bulk-Only Transport implementation.

## Architecture

```
host -> USB 2.0 -> FunctionFS (kernel) -> src/usb/bot.c
     -> src/core/scsi.c (MMC command subset)
     -> src/core/media.c (image state machine, .meta sidecar)
     -> src/posix/ffshim.c / storage_posix.c (pread/pwrite/FatFs shim)
     -> src/ui/ (OLED, EC11, menus)
```

Key files:

| File | Responsibility |
|---|---|
| `src/core/scsi.c` | MMC commands, sense, profiles, format/TOC/track/event |
| `src/core/media.c` | image open, blank/format/write/close, `.meta` |
| `src/core/media_meta.h` | 48-byte little-endian metadata, v2 |
| `src/usb/bot.c` | CBW/Data/CSW, failed-IN data stage handling |
| `src/usb/ffs_gadget.c` | configfs/FunctionFS setup, descriptors |
| `src/posix/*` | Linux file and directory backend |
| `src/ui/*` | OLED/encoder menu, disc management |
| `tests/*` | host-side regression tests |

## Invariants and hard-won knowledge

1. **Do not return `READ CAPACITY = 0` for a blank CD-RW.** Windows uses
   the format-capacity descriptors; 0 makes Explorer refuse the disc.
   The physical capacity is 358400 sectors; the formatted descriptor is
   295264 sectors.
2. **CD-RW FORMAT UNIT is asynchronous.** Return GOOD, then TUR must
   report `2/04/04` (format in progress), then READY. BLANK reports
   `2/04/07`. The daemon currently simulates realistic durations.
3. **A4 REPORT KEY must succeed** with the captured 8-byte response
   `00 06 00 00 25 ff 01 00`; returning CHECK CONDITION causes USB reset.
4. **GET EVENT** must advertise classes `0x5E`; class `0x5A` returns the
   8-byte media event `00 06 06 5E ...`. Without this Windows/IMAPI does
   not poll format progress.
5. **Disc Information:** blank CD-RW = 36 bytes; formatted complete = 34
   bytes and `0x1E` (COMPLETE/state 3), bytes 12–15 `00 02 99 00`.
6. **TOC:** CD-RW control byte `0x15`. Format 0 lead-out `359336`,
   format 2 lead-out `359848` (MSF `4F:3B:49`). Before UDF data exists,
   TOC formats 0/1/2 fail; this prevents Windows looping on TOC.
7. **Track Information:** blank CD-RW 48 bytes, flags `4F/01`,
   NWA near lead-out; complete CD-RW 36 bytes, flags `B1/02`.
8. **CLOSE TRACK/SESSION:** function 2 closes the session **without**
   finalizing; function 3 finalizes. Finalizing on function 2 breaks
   ImgBurn multi-session.
9. `/opt` may run a stale binary after a source edit. Always check
   `ls -l /opt/pi3b-cdrw/build/pi3b-cdrw-emu` and reinstall before
   debugging. This caused a long false trail.
10. Do not commit `discs/`, `build/`, captures (`*.pcapng`) or secrets.

## Current known issue (P0)

Windows built-in **Live File System** formatting reports
“Windows 无法完成格式化”. The Windows format engine completes
FORMAT UNIT, waits through the simulated format, then never issues
`READ TRACK` or `WRITE(10)`; it falls back to Explorer TOC polling.
The same image works with ImgBurn (erase/burn/verify) and Linux
`wodim`/`growisofs`. See `TODO.MD` and `docs/windows-capture.md`.

Do not “fix” this by pre-formatting a generic UDF image: Windows treats
mkudffs UDF and even a copied Windows UDF as read-only/finalized and
shows 0 bytes free. The real capture and the failing capture are in the
history/captures referenced by the docs; only the true WRITE path fixes it.

## Sibling sync

CD-RW-only changes already ported to `rp2350-cdrw-emu`:
async BLANK/FORMAT, A4, GET EVENT 0x5E/0x5A, DiscInfo 34/36, TOC
control/lead-out, Track 36/48, format capacity 295264, meta v2 and
non-finalizing close. DVD/BD and FunctionFS code are intentionally not
ported. Keep the two implementations behaviorally aligned where possible.

## Working style

- Keep changes small and testable; run `make test` after every behavior
  change.
- Add a regression test for every SCSI field change if practical.
- Prefer real USBPcap evidence (`tools/usbpcap_scsi.py`) over guesses.
- Do not add network dependencies to the daemon.
- Keep the project usable on a fresh Debian 13 install.
