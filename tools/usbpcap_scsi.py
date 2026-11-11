#!/usr/bin/env python3
"""Decode SCSI commands from a USBPcap USB Mass Storage BOT capture.

The Windows USB storage stack wraps every SCSI CDB in a Bulk-Only
Transport CBW, followed by the data stage and a CSW.  tshark's USBPcap
dissector exposes the CBW tag/lengths and the raw SCSI payload, which is
enough to rebuild the command sequence and inspect the response bytes.

Requires tshark (Wireshark) with USBPcap support.

Examples:
    tools/usbpcap_scsi.py CD-RW.pcapng --summary
    tools/usbpcap_scsi.py CD-RW.pcapng --op 0x46 --hex
    tools/usbpcap_scsi.py CD-RW.pcapng --op 0xac,0xad --hex
"""
import argparse
import collections
import struct
import subprocess
import sys

FIELDS = [
    "frame.number", "frame.time_relative", "usb.src", "usb.dst",
    "usb.endpoint_address", "usb.device_address", "usb.data_len",
    "usbms.dCBWSignature", "usbms.dCBWTag",
    "usbms.dCBWDataTransferLength", "usbms.dCBWFlags",
    "usbms.dCBWCBLength",
    "usbms.dCSWSignature", "usbms.dCSWDataResidue",
    "usbms.dCSWStatus", "data.data",
]

OP_NAMES = {
    0x00: "TEST UNIT READY", 0x03: "REQUEST SENSE", 0x12: "INQUIRY",
    0x1B: "START STOP UNIT", 0x1E: "PREVENT ALLOW",
    0x23: "READ FORMAT CAPACITIES", 0x25: "READ CAPACITY",
    0x28: "READ(10)", 0x2A: "WRITE(10)", 0x2E: "WRITE AND VERIFY",
    0x35: "SYNCHRONIZE CACHE", 0x43: "READ TOC/PMA/ATIP",
    0x46: "GET CONFIGURATION", 0x4A: "GET EVENT STATUS",
    0x51: "READ DISC INFORMATION", 0x52: "READ TRACK INFORMATION",
    0x53: "RESERVE TRACK", 0x54: "SEND OPC", 0x55: "MODE SELECT",
    0x5A: "MODE SENSE", 0x5B: "CLOSE TRACK/SESSION",
    0x5C: "READ BUFFER CAPACITY", 0x5D: "SEND CUE SHEET",
    0xAC: "GET PERFORMANCE", 0xAD: "READ DISC STRUCTURE",
    0xA1: "BLANK", 0xA3: "SEND KEY", 0xA4: "REPORT KEY",
    0xBF: "SEND DVD STRUCTURE",
}


def usb_payloads(path):
    """Return {frame_number: USB payload} from a pcapng/EPB capture.

    tshark consumes the SCSI CDB for known commands and does not always
    export the raw bytes as data.data.  The pcapng packet still contains
    the USBPcap pseudoheader followed by the 31-byte CBW, so read it
    directly for those commands.
    """
    raw = open(path, "rb").read()
    out = {}
    off = 0
    frame = 0
    endian = "<"
    while off + 12 <= len(raw):
        btype = struct.unpack_from(endian + "I", raw, off)[0]
        blen = struct.unpack_from(endian + "I", raw, off + 4)[0]
        if blen < 12 or off + blen > len(raw):
            break
        if btype == 0x0A0D0D0A and off + 12 <= len(raw):
            magic = raw[off + 8:off + 12]
            endian = "<" if magic == b"\x4d\x3c\x2b\x1a" else ">"
        elif btype == 6 and off + 28 <= len(raw):
            iface, ts_hi, ts_lo, caplen, origlen = struct.unpack_from(
                endian + "IIIII", raw, off + 8)
            pkt = raw[off + 28:off + 28 + caplen]
            frame += 1
            if len(pkt) >= 2:
                hlen = struct.unpack_from("<H", pkt, 0)[0]
                if 27 <= hlen <= len(pkt):
                    out[frame] = pkt[hlen:]
        off += blen
    return out


def read_rows(path):
    cmd = ["tshark", "-r", path, "-T", "fields",
           "-E", "separator=/t", "-E", "occurrence=f"]
    for f in FIELDS:
        cmd += ["-e", f]
    out = subprocess.run(cmd, check=True, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    for line in out.stdout.splitlines():
        p = line.split("\t")
        p += [""] * (len(FIELDS) - len(p))
        yield dict(zip(FIELDS, p))


def parse(path, address=None):
    txns = []
    cur = None
    payloads = usb_payloads(path)
    for r in read_rows(path):
        frame = int(r["frame.number"])
        addr = int(r["usb.device_address"] or 0, 0)
        if address is not None and addr != address:
            continue
        payload = payloads.get(frame, b"")
        if r["usbms.dCBWSignature"]:
            cdb = bytes.fromhex(r["data.data"].replace(":", ""))
            if not cdb and payload[:4] == b"USBC":
                cb_len = int(r["usbms.dCBWCBLength"] or 0, 0)
                cdb = payload[15:15 + cb_len]
            cur = {
                "frame": int(r["frame.number"]),
                "time": float(r["frame.time_relative"]),
                "addr": addr,
                "tag": r["usbms.dCBWTag"],
                "dtl": int(r["usbms.dCBWDataTransferLength"] or 0, 0),
                "flags": int(r["usbms.dCBWFlags"] or 0, 0),
                "cdb": cdb,
                "data": bytearray(),
                "status": None,
                "residue": None,
            }
            txns.append(cur)
        elif r["usbms.dCSWSignature"]:
            if cur is not None:
                cur["status"] = int(r["usbms.dCSWStatus"] or 0, 0)
                cur["residue"] = int(r["usbms.dCSWDataResidue"] or 0, 0)
                cur["csw_frame"] = int(r["frame.number"])
                cur = None
        elif cur is not None:
            if r["data.data"]:
                try:
                    cur["data"] += bytes.fromhex(r["data.data"].replace(":", ""))
                except ValueError:
                    pass
            elif payload and payload[:4] not in (b"USBC", b"USBS"):
                cur["data"] += payload
    return txns


def opname(op):
    return OP_NAMES.get(op, "opcode 0x%02x" % op)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap")
    ap.add_argument("--op", action="append", default=[],
                    help="opcode filter, e.g. 0x46 or 46 (repeatable/comma list)")
    ap.add_argument("--hex", action="store_true", help="print response bytes")
    ap.add_argument("--limit", type=int, default=0, help="limit printed commands")
    ap.add_argument("--addr", type=int, default=None,
                    help="only decode this USB device address (USBPcap bus address)")
    ap.add_argument("--summary", action="store_true", help="only print counts")
    args = ap.parse_args()

    wanted = set()
    for item in args.op:
        for part in item.replace(",", " ").split():
            wanted.add(int(part, 0))

    txns = parse(args.pcap, args.addr)
    if args.summary:
        print("transactions:", len(txns),
              ("address %d" % args.addr) if args.addr is not None else "all addresses")
        for op, n in collections.Counter(t["cdb"][0] for t in txns if t["cdb"]).most_common():
            print("  %02x %-24s %4d" % (op, opname(op), n))
        print("status:", dict(collections.Counter(t["status"] for t in txns)))
        return

    printed = 0
    for t in txns:
        if not t["cdb"]:
            continue
        if wanted and t["cdb"][0] not in wanted:
            continue
        print("frame %-5d t=%8.3f op=%02x %-24s cdb=%s dtl=%d got=%d status=%s res=%s"
              % (t["frame"], t["time"], t["cdb"][0], opname(t["cdb"][0]),
                 t["cdb"].hex(), t["dtl"], len(t["data"]),
                 t["status"], t["residue"]))
        if args.hex and t["data"]:
            d = bytes(t["data"])
            for off in range(0, len(d), 16):
                print("    %04x  %s" % (off, d[off:off + 16].hex(" ")))
        printed += 1
        if args.limit and printed >= args.limit:
            break


if __name__ == "__main__":
    try:
        main()
    except FileNotFoundError:
        sys.exit("tshark not found; install tshark/wireshark")
    except subprocess.CalledProcessError as e:
        sys.exit(e.stderr.decode() if isinstance(e.stderr, bytes) else str(e.stderr))
