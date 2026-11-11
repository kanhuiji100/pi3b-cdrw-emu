# TODO — Orange Pi 3B CD/DVD/BD emulator

**Positioning:** high-capacity / high-performance USB optical emulator
(USB 2.0, CD-RW + DVD-RW + BD-RE). For the low-cost plug-and-play
USB 1.1 CD-RW sibling, see `rp2350-cdrw-emu`.

## P0 — Windows Live File System format

- [ ] Make Windows Explorer “类似于 U 盘” formatting complete and leave a
      writable live-file-system disc.
- Current evidence: Windows reaches 100% of the 576 MB format, then reports
  “Windows 无法完成格式化”; the capture contains no `READ TRACK (52)` and
  no `WRITE(10) (2A)` afterwards. ImgBurn and Linux tools work.
- [ ] Capture a successful **same Windows Explorer live-FS** format on the
      real drive (not ImgBurn) and diff the post-FORMAT sequence command by
      command.
- [ ] Focus on why the Windows formatter chooses Explorer TOC polling
      instead of `READ TRACK` after FORMAT. Do not add more synthetic TOC
      fields without a capture proving the need.
- [ ] Keep the 10-minute simulated format or reduce only after the Windows
      path truly reaches WRITE.

## P1 — Blank-disc capacity and UX

- [ ] Blank CD-RW currently shows 0 bytes in Explorer on some Windows
      builds even though the burn dialog opens. Real blank media shows
      ~702 MB. Re-check `READ FORMAT CAPACITIES` descriptor 0 and
      `READ CAPACITY` behavior against a Windows capture.
- [ ] Verify that a blank disc still opens the “刻录光盘” dialog after any
      further TOC/Track changes.
- [ ] OLED menu: confirm delete/erase/new/load flows on the latest binary.

## P1 — Multi-session / ImgBurn

- [x] `CLOSE TRACK/SESSION` function 2 no longer finalizes; function 3
      finalizes.
- [ ] Re-test ImgBurn multi-session append; verify volume label, used/free
      capacity and a second session.
- [ ] Add a host test for close-session semantics.

## P2 — Engineering

- [ ] Add a regression test for the real `A4 REPORT KEY` response.
- [ ] Add a test for GET EVENT `0x5A/0x5E` and format-in-progress TUR.
- [ ] Document how to use `docs/captures` (captures are intentionally not
      committed).
- [ ] Add a small `systemd` health check/status command.
- [ ] Consider a control socket for image switching without restarting.

## Sync status with rp2350-cdrw-emu

Ported:
- [x] A4 REPORT KEY real response
- [x] GET EVENT 0x5E/0x5A
- [x] async BLANK/FORMAT (TUR 2/04/07 and 2/04/04)
- [x] DiscInfo 34/36, complete 0x1E, last-session fields
- [x] TOC control 0x15 and format 0/2 lead-outs
- [x] Track blank 48 / complete 36
- [x] format capacity 295264 and physical descriptor 358400
- [x] meta v2 (`FORMATTED`) and non-finalizing close session

Not ported (intentionally or pending):
- [ ] FunctionFS/Linux USB2.0 stack (not applicable to RP2350)
- [ ] DVD-RW / BD-RE profiles and Disc Structure (RP2350 is CD-RW only)
- [ ] BOT failed-IN short/zero-stage strategy (TinyUSB path differs)
- [ ] Real INQUIRY identity (RP2350 keeps its own identity)

## Known non-goals

- Windows generic UDF pre-format is **not** a fix; Windows treats it as
  read-only/finalized (0 bytes free).
- Do not commit captured `.pcapng` files, `discs/`, or build artifacts.
