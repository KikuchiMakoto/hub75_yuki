#!/usr/bin/env python3

import argparse
import sys
import time

try:
    import serial
except ImportError as exc:
    raise SystemExit("pyserial is required: pip install pyserial") from exc


def parse_stat_line(line: str) -> dict[str, int]:
    out: dict[str, int] = {}
    if not line.startswith("STAT "):
        return out
    for token in line[5:].split():
        if "=" not in token:
            continue
        k, v = token.split("=", 1)
        try:
            out[k] = int(v)
        except ValueError:
            pass
    return out


def cobs_encode(data: bytes) -> bytes:
    if not data:
        return b""

    out = bytearray()
    code_pos = 0
    out.append(0)
    code = 1

    for b in data:
        if b == 0:
            out[code_pos] = code
            code_pos = len(out)
            out.append(0)
            code = 1
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_pos] = code
                code_pos = len(out)
                out.append(0)
                code = 1

    out[code_pos] = code
    return bytes(out)


def rgb888_to_rgb565_le(r: int, g: int, b: int) -> tuple[int, int]:
    p = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return p & 0xFF, (p >> 8) & 0xFF


def build_frame_rgb565(frame_index: int, width: int, height: int) -> bytes:
    data = bytearray(width * height * 2)
    pos = 0

    for y in range(height):
        for x in range(width - 1, -1, -1):
            r = (x * 3 + frame_index * 5) & 0xFF
            g = (y * 9 + frame_index * 3) & 0xFF
            b = ((x ^ y) * 7 + frame_index * 11) & 0xFF
            lo, hi = rgb888_to_rgb565_le(r, g, b)
            data[pos] = lo
            data[pos + 1] = hi
            pos += 2

    return bytes(data)


def build_packet_cache(width: int, height: int, cache_frames: int) -> list[bytes]:
    if cache_frames < 1:
        cache_frames = 1

    packets: list[bytes] = []
    for i in range(cache_frames):
        raw = build_frame_rgb565(i, width, height)
        packets.append(cobs_encode(raw) + b"\x00")
    return packets


def run_benchmark(args: argparse.Namespace) -> int:
    frame_bytes = args.width * args.height * 2
    if frame_bytes <= 0:
        print("invalid frame size", file=sys.stderr)
        return 2

    interval = 1.0 / args.fps if args.fps > 0 else 0.0
    packet_cache = build_packet_cache(args.width, args.height, args.cache_frames)

    ser = serial.Serial(
        port=args.port,
        baudrate=args.baud,
        timeout=0.001,
        write_timeout=0.05,
    )

    try:
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        time.sleep(args.warmup)

        start = time.perf_counter()
        end = start + args.duration
        next_send = start
        last_report = start

        frame_index = 0
        sent_frames = 0
        sent_bytes = 0
        send_errors = 0

        prev_sent_frames = 0
        prev_sent_bytes = 0
        last_device_stat = "STAT unavailable"
        stat_samples: list[dict[str, int]] = []

        print(
            f"bench start port={args.port} width={args.width} height={args.height} "
            f"target_fps={args.fps:.2f} duration={args.duration:.1f}s cache_frames={len(packet_cache)}"
        )

        while True:
            now = time.perf_counter()
            if now >= end:
                break

            while interval > 0 and now >= next_send:
                pkt = packet_cache[frame_index % len(packet_cache)]
                try:
                    written = ser.write(pkt)
                    if written is None:
                        written = 0
                    sent_bytes += written
                    if written == len(pkt):
                        sent_frames += 1
                    else:
                        send_errors += 1
                    frame_index += 1
                except serial.SerialTimeoutException:
                    send_errors += 1

                next_send += interval
                now = time.perf_counter()

            while ser.in_waiting:
                line = ser.readline().decode("ascii", errors="ignore").strip()
                if line.startswith("STAT "):
                    last_device_stat = line
                    parsed = parse_stat_line(line)
                    if parsed:
                        stat_samples.append(parsed)

            if (now - last_report) >= 1.0:
                one_sec_frames = sent_frames - prev_sent_frames
                one_sec_bytes = sent_bytes - prev_sent_bytes
                prev_sent_frames = sent_frames
                prev_sent_bytes = sent_bytes
                last_report = now

                print(
                    f"HOST tx_Bps={one_sec_bytes} send_fps={one_sec_frames} send_err={send_errors} | {last_device_stat}"
                )

            time.sleep(0.0005)

        elapsed = time.perf_counter() - start
        avg_fps = sent_frames / elapsed if elapsed > 0 else 0.0
        avg_bps = sent_bytes / elapsed if elapsed > 0 else 0.0

        print(
            f"bench done elapsed={elapsed:.2f}s sent_frames={sent_frames} sent_bytes={sent_bytes} "
            f"avg_send_fps={avg_fps:.2f} avg_tx_Bps={avg_bps:.1f} send_err={send_errors}"
        )
        print(last_device_stat)

        if stat_samples:
            avg_fields = [
                "clk_khz",
                "dec_fps",
                "disp_fps",
                "scan_fps",
                "drop_ps",
                "usb_drop_Bps",
                "tud_gap_us",
                "dec_us",
                "conv_us",
                "usb_hw",
                "pkt_hw",
            ]
            summary = []
            for f in avg_fields:
                vals = [s[f] for s in stat_samples if f in s]
                if vals:
                    summary.append(f"{f}_avg={sum(vals)/len(vals):.2f}")
            if summary:
                print("BENCH " + " ".join(summary))

        return 0
    finally:
        ser.close()


def main() -> int:
    p = argparse.ArgumentParser(description="USB CDC benchmark sender for firmware_v2")
    p.add_argument("--port", required=True, help="Serial port (example: COM5 or /dev/ttyACM0)")
    p.add_argument("--width", type=int, default=128)
    p.add_argument("--height", type=int, default=32)
    p.add_argument("--fps", type=float, default=140.0)
    p.add_argument("--duration", type=float, default=20.0)
    p.add_argument("--baud", type=int, default=2000000, help="Ignored by USB CDC but required by pyserial")
    p.add_argument("--warmup", type=float, default=0.3)
    p.add_argument(
        "--cache-frames",
        type=int,
        default=32,
        help="Number of pre-encoded frames reused in a loop (higher reduces host CPU load)",
    )
    args = p.parse_args()
    return run_benchmark(args)


if __name__ == "__main__":
    raise SystemExit(main())
