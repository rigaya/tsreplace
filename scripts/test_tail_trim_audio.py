#!/usr/bin/env python3
"""末尾トリムで後方の音声PESを保持し、AACとBフレーム入り映像の復号を確認する。"""

import argparse
import subprocess
from pathlib import Path


def run(args, stdin=None):
    result = subprocess.run(args, stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    return result


def packets(data):
    assert len(data) % 188 == 0, "TSパケットの末尾が欠けています"
    for offset in range(0, len(data), 188):
        packet = data[offset:offset + 188]
        assert packet[0] == 0x47, "TSの同期が失われています"
        yield packet


def pid(packet):
    return ((packet[1] & 31) << 8) | packet[2]


def add_audio_tags(data):
    # 日本の放送TSと同じcomponent_tagで、第1・第2音声を明示する。
    result = bytearray()
    for packet in packets(data):
        if pid(packet) == 4096 and packet[1] & 0x40:
            offset = 4 + (1 + packet[4] if packet[3] & 0x20 else 0)
            offset += 1 + packet[offset]
            length = ((packet[offset + 1] & 15) << 8) | packet[offset + 2]
            table = bytearray(packet[offset:offset + 3 + length - 4])
            pos = 12 + (((table[10] & 15) << 8) | table[11])
            while pos < len(table):
                stream_pid = ((table[pos + 1] & 31) << 8) | table[pos + 2]
                info_size = ((table[pos + 3] & 15) << 8) | table[pos + 4]
                if stream_pid in (257, 258):
                    table[pos + 5 + info_size:pos + 5 + info_size] = bytes([0x52, 1, 0x10 + stream_pid - 257])
                    info_size += 3
                    table[pos + 3:pos + 5] = (0xf000 | info_size).to_bytes(2, "big")
                pos += 5 + info_size
            table[1:3] = (0xb000 | (len(table) + 4 - 3)).to_bytes(2, "big")
            crc = 0xffffffff
            for value in table:
                crc ^= value << 24
                for _ in range(8):
                    crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
            table.extend(crc.to_bytes(4, "big"))
            assert offset + len(table) <= 188, "PMTが1パケットに収まりません"
            packet = packet[:offset] + table + b"\xff" * (188 - offset - len(table))
        result.extend(packet)
    return bytes(result)


def read_pes(data, target_pid):
    result = []
    for packet in packets(data):
        if pid(packet) != target_pid or not packet[3] & 0x10:
            continue
        offset = 4 + (1 + packet[4] if packet[3] & 0x20 else 0)
        payload = packet[offset:]
        if not payload:
            continue
        if packet[1] & 0x40:
            assert payload[:3] == b"\x00\x00\x01", "PESヘッダがありません"
            assert payload[7] & 0x80, "PESにPTSがありません"
            b = payload[9:14]
            pts = ((b[0] >> 1) & 7) << 30 | b[1] << 22 | (b[2] >> 1) << 15 | b[3] << 7 | b[4] >> 1
            result.append([pts, bytearray()])
        if result:
            result[-1][1].extend(payload)
    return result


def es_payload(pes):
    size = int.from_bytes(pes[4:6], "big")
    assert size == 0 or len(pes) == size + 6, "PESが途中で切れています"
    return bytes(pes[9 + pes[8]:])


def check_audio(source, output, audio_pids, boundary):
    for audio_pid in audio_pids:
        expected = [p for p in read_pes(source, audio_pid) if p[0] < boundary]
        actual = read_pes(output, audio_pid)
        assert [p[0] for p in actual] == [p[0] for p in expected], f"音声PESが欠落しています: {audio_pid:#x}"
        for index, ((_, original), (_, replaced)) in enumerate(zip(expected, actual)):
            original_es, replaced_es = es_payload(original), es_payload(replaced)
            if index + 1 < len(expected):
                assert replaced_es == original_es, "末尾以外の音声が変更されています"
            else:
                assert original_es.startswith(replaced_es), "最後の音声PESが一致しません"
                # 末尾の未完結ADTSフレームだけを除いたことを確認する。
                offset = 0
                while offset < len(replaced_es):
                    header = replaced_es[offset:offset + 7]
                    assert len(header) == 7 and header[0] == 255 and header[1] & 0xf6 == 0xf0
                    length = ((header[3] & 3) << 11) | header[4] << 3 | header[5] >> 5
                    assert length >= 7
                    offset += length
                assert offset == len(replaced_es), "最後のAACフレームが途中で切れています"
                if len(original_es) > offset:
                    header = original_es[offset:offset + 7]
                    length = ((header[3] & 3) << 11) | header[4] << 3 | header[5] >> 5
                    assert len(original_es) - offset < length, "完全なAACフレームが欠落しています"
        print(f"PID {audio_pid:#x}: {len(actual)}個の音声PESを確認")


def decode_video_hashes(ffmpeg, path):
    decoded = run(ffmpeg + ["-v", "warning", "-i", str(path), "-map", "0:v:0",
                            "-fps_mode", "passthrough", "-f", "framemd5", "-"])
    assert not decoded.stderr, decoded.stderr.decode(errors="replace")
    return [line.rsplit(",", 1)[-1].strip() for line in decoded.stdout.decode().splitlines()
            if line and not line.startswith("#")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tsreplace", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    root = args.work_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    source, replacement = root / "source.ts", root / "replacement.mp4"
    ffmpeg = [args.ffmpeg, "-v", "error", "-nostdin", "-y"]
    run(ffmpeg + ["-f", "lavfi", "-i", "testsrc2=size=160x90:rate=30",
                  "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                  "-t", "6", "-map", "0:v", "-map", "1:a", "-map", "1:a",
                  "-c:v", "mpeg2video", "-bf", "0", "-c:a", "aac", "-b:a", "128k",
                  "-streamid", "0:256", "-streamid", "1:257", "-streamid", "2:258", str(source)])
    # 置換映像を境界より長くし、音声待ち中も映像の書き出しが境界付近で止まることを確認する。
    run(ffmpeg + ["-i", str(source), "-an", "-c:v", "libx264", "-preset", "ultrafast", str(replacement)])
    # 境界前のBフレームが境界以降の参照フレームに依存する並びを固定する。
    replacement_b = root / "replacement_b.mp4"
    run(ffmpeg + ["-i", str(source), "-an", "-c:v", "libx264", "-preset", "medium",
                  "-x264-params", "bframes=3:b-adapt=0:keyint=250:scenecut=0", str(replacement_b)])
    reference_hashes = {path: decode_video_hashes(ffmpeg, path)
                        for path in (replacement, replacement_b)}
    original = add_audio_tags(source.read_bytes())
    boundary = read_pes(original, 256)[0][0] + 3 * 90000
    cut = root / "tail.txt"
    cut.write_text(f"# tsreplace-cut-v2\ntimebase=90000\ncut {boundary} -1\n")
    # 音声PIDごとに異なる遅れを作り、最初の音声境界だけで終了する実装も検出する。
    ordered = sorted(enumerate(packets(original)), key=lambda item:
                     item[0] + {257: 200, 258: 1000}.get(pid(item[1]), 0))
    delayed = b"".join(packet for _, packet in ordered)
    # 最後のkeep PESの次にPUSIがない場合は、入力EOFで確定する。
    eof_packets = []
    keep = {257: True, 258: True}
    for packet in packets(delayed):
        packet_pid = pid(packet)
        if packet_pid in keep and packet[1] & 0x40:
            keep[packet_pid] = read_pes(packet, packet_pid)[0][0] < boundary
        if keep.get(packet_pid, True):
            eof_packets.append(packet)
    cases = [("file", delayed, [257, 258], False, replacement),
             ("stdin", delayed, [257, 258], True, replacement),
             ("eof", b"".join(eof_packets), [257, 258], False, replacement),
             ("b_frames", delayed, [257, 258], False, replacement_b)]
    no_audio = root / "no_audio.ts"
    run(ffmpeg + ["-i", str(source), "-map", "0:v", "-c", "copy", str(no_audio)])
    cases.append(("no_audio", no_audio.read_bytes(), [], False, replacement))
    for name, data, audio_pids, use_stdin, replace_path in cases:
        input_path, output = root / f"{name}_input.ts", root / f"{name}_output.ts"
        input_path.write_bytes(data)
        command = [str(args.tsreplace.resolve()), "-i", "-" if use_stdin else str(input_path),
                   "-r", str(replace_path), "--replace-format", "mp4", "--cut-list", str(cut), "-o", str(output)]
        with input_path.open("rb") as stream:
            result = run(command, stdin=stream if use_stdin else None)
        (root / f"{name}.log").write_bytes(result.stderr)
        output_data = output.read_bytes()
        check_audio(data, output_data, audio_pids, boundary)
        video = read_pes(output_data, 256)
        # 30fps・最大3枚のBフレームに必要な参照映像と、DTSが境界と等しい1枚を許容する。
        assert video and max(p[0] for p in video) <= boundary + 3 * 3000, "映像が末尾境界を大幅に超えています"
        if name == "b_frames":
            assert any(a[0] > b[0] for a, b in zip(video, video[1:])), "Bフレームの並べ替えがありません"
            assert any(p[0] >= boundary for p in video), "境界以降の参照映像が欠落しています"
        actual_hashes = decode_video_hashes(ffmpeg, output)
        assert 90 <= len(actual_hashes) <= 94, "境界前の映像が欠落するか、末尾が延びすぎています"
        assert actual_hashes == reference_hashes[replace_path][:len(actual_hashes)], "映像の復号結果が元置換映像と一致しません"
        if audio_pids:
            decoded = run(ffmpeg + ["-xerror", "-i", str(output), "-map", "0:a", "-f", "null", "-"])
            assert not decoded.stderr, decoded.stderr.decode(errors="replace")
        print(f"{name}: 合格")


if __name__ == "__main__":
    main()
