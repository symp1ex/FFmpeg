#!/usr/bin/env python3
"""Windows integration tests for ffmpeg's runtime control inputs."""

import argparse
import msvcrt
import os
import subprocess
import sys
import time


WIDTH = 64
HEIGHT = 64
FRAME_SIZE = WIDTH * HEIGHT * 3 // 2


def encoder_args(input_url):
    return [
        "-f", "rawvideo",
        "-pix_fmt", "yuv420p",
        "-video_size", f"{WIDTH}x{HEIGHT}",
        "-framerate", "10",
        "-i", input_url,
        "-an",
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-tune", "zerolatency",
        "-profile:v", "baseline",
        "-threads", "1",
        "-g", "1000",
        "-keyint_min", "1000",
        "-sc_threshold", "0",
        "-x264-params", "repeat-headers=1:aud=1",
        "-f", "h264",
        "pipe:1",
    ]


def frame(index):
    y = bytes([(32 + index * 11) & 0xFF]) * (WIDTH * HEIGHT)
    uv = bytes([96 + index % 16, 160 - index % 16]) * (WIDTH * HEIGHT // 4)
    data = y + uv
    if len(data) != FRAME_SIZE:
        raise AssertionError(f"bad test frame size: {len(data)}")
    return data


def write_fd(fd, data):
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        if written <= 0:
            raise OSError("short write to media pipe")
        view = view[written:]


def finish_process(proc, timeout=10):
    try:
        output, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        output, stderr = proc.communicate()
        diagnostic = stderr.decode("utf-8", "replace")
        raise AssertionError(f"ffmpeg pid {proc.pid} did not exit: {diagnostic}")

    return output, stderr.decode("utf-8", "replace")


def annexb_access_units(data):
    starts = []
    pos = 0
    while pos + 3 < len(data):
        if data[pos:pos + 4] == b"\x00\x00\x00\x01":
            starts.append((pos, pos + 4))
            pos += 4
        elif data[pos:pos + 3] == b"\x00\x00\x01":
            starts.append((pos, pos + 3))
            pos += 3
        else:
            pos += 1

    nal_types = []
    for index, (_, payload) in enumerate(starts):
        end = starts[index + 1][0] if index + 1 < len(starts) else len(data)
        if payload < end:
            nal_types.append(data[payload] & 0x1F)

    access_units = []
    current = []
    for nal_type in nal_types:
        if nal_type == 9 and current:
            access_units.append(current)
            current = []
        current.append(nal_type)
    if current:
        access_units.append(current)
    return access_units


def assert_h264(output, minimum_frames, minimum_idrs):
    access_units = annexb_access_units(output)
    video_units = [unit for unit in access_units if 1 in unit or 5 in unit]
    idr_units = [unit for unit in video_units if 5 in unit]
    if len(video_units) < minimum_frames:
        raise AssertionError(
            f"encoded only {len(video_units)} access units, expected at least {minimum_frames}"
        )
    if len(idr_units) < minimum_idrs:
        raise AssertionError(
            f"encoded only {len(idr_units)} IDR access units, expected at least {minimum_idrs}; "
            f"NAL units={video_units}"
        )


def base_command(ffmpeg):
    return [ffmpeg, "-hide_banner", "-loglevel", "warning", "-nostats"]


def test_without_control_channel(ffmpeg):
    command = base_command(ffmpeg) + ["-nostdin"] + encoder_args("pipe:0")
    proc = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    for index in range(6):
        proc.stdin.write(frame(index))
    proc.stdin.close()
    proc.stdin = None
    output, stderr = finish_process(proc)
    if proc.returncode != 0:
        raise AssertionError(f"ffmpeg without control channel failed: {stderr}")
    assert_h264(output, minimum_frames=6, minimum_idrs=1)


def test_invalid_control_handle(ffmpeg):
    command = (
        base_command(ffmpeg)
        + ["-nostdin", "-runtime_control_handle", "invalid"]
        + encoder_args("pipe:0")
    )
    proc = subprocess.run(
        command,
        input=b"",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
        check=False,
    )
    stderr = proc.stderr.decode("utf-8", "replace")
    if proc.returncode == 0 or "Invalid runtime control handle 'invalid'" not in stderr:
        raise AssertionError(
            f"invalid control handle was not rejected: code={proc.returncode} stderr={stderr}"
        )


def test_legacy_stdin_control(ffmpeg):
    command = base_command(ffmpeg) + [
        "-re",
        "-f", "gdigrab",
        "-framerate", "10",
        "-video_size", f"{WIDTH}x{HEIGHT}",
        "-i", "desktop",
        "-an",
        "-vf", "format=yuv420p",
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-tune", "zerolatency",
        "-profile:v", "baseline",
        "-threads", "1",
        "-g", "1000",
        "-keyint_min", "1000",
        "-sc_threshold", "0",
        "-frames:v", "20",
        "-x264-params", "repeat-headers=1:aud=1",
        "-f", "h264",
        "pipe:1",
    ]
    proc = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    original_pid = proc.pid

    time.sleep(0.5)
    if proc.poll() is not None:
        output, stderr = finish_process(proc)
        raise AssertionError(f"legacy ffmpeg exited before the command: {stderr}")
    proc.stdin.write(b"force_keyframe\n")
    proc.stdin.flush()
    time.sleep(0.06)
    if proc.pid != original_pid or proc.poll() is not None:
        raise AssertionError("legacy force_keyframe changed or stopped the ffmpeg process")

    output, stderr = finish_process(proc)
    if proc.returncode != 0:
        raise AssertionError(f"legacy stdin control failed: {stderr}")
    if stderr.count("force_keyframe command received") != 1:
        raise AssertionError(f"legacy command was not acknowledged exactly once: {stderr}")
    if stderr.count("Runtime force_keyframe: next video frame forced") != 1:
        raise AssertionError(f"legacy encoder request was not consumed exactly once: {stderr}")
    assert_h264(output, minimum_frames=20, minimum_idrs=2)


def test_dedicated_control_handle(ffmpeg):
    control_read, control_write = os.pipe()
    control_handle = msvcrt.get_osfhandle(control_read)
    os.set_handle_inheritable(control_handle, True)
    command = (
        base_command(ffmpeg)
        + ["-loglevel", "info", "-nostdin", "-runtime_control_handle", str(control_handle)]
        + encoder_args("pipe:0")
    )
    startup_info = subprocess.STARTUPINFO()
    startup_info.lpAttributeList = {"handle_list": [control_handle]}
    proc = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        close_fds=True,
        startupinfo=startup_info,
    )
    os.close(control_read)
    original_pid = proc.pid

    try:
        for index in range(3):
            proc.stdin.write(frame(index))
            proc.stdin.flush()
            time.sleep(0.06)

        for request_at in (3, 8):
            write_fd(control_write, b"force_keyframe\r\n")
            time.sleep(0.06)
            if proc.pid != original_pid or proc.poll() is not None:
                raise AssertionError("dedicated force_keyframe changed or stopped the ffmpeg process")

            end = request_at + 5
            for index in range(request_at, end):
                proc.stdin.write(frame(index))
                proc.stdin.flush()
                time.sleep(0.06)

        os.close(control_write)
        control_write = None
        time.sleep(0.06)
        if proc.poll() is not None:
            raise AssertionError("closing the control channel stopped ffmpeg")

        for index in range(13, 16):
            proc.stdin.write(frame(index))
            proc.stdin.flush()
            time.sleep(0.06)
    finally:
        if control_write is not None:
            os.close(control_write)
        proc.stdin.close()
        proc.stdin = None

    output, stderr = finish_process(proc)
    if proc.returncode != 0:
        raise AssertionError(f"dedicated runtime control failed: {stderr}")
    if stderr.count("force_keyframe command received") != 2:
        raise AssertionError(f"dedicated commands were not acknowledged twice: {stderr}")
    if stderr.count("Runtime force_keyframe: next video frame forced") != 2:
        raise AssertionError(f"dedicated encoder requests were not consumed twice: {stderr}")
    if "Runtime control channel closed" not in stderr:
        raise AssertionError(f"control EOF was not acknowledged: {stderr}")
    if "Failed to close runtime control channel" in stderr:
        raise AssertionError(f"control handle close failed: {stderr}")
    assert_h264(output, minimum_frames=16, minimum_idrs=3)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ffmpeg", help="path to the ffmpeg executable under test")
    parser.add_argument(
        "--skip-legacy",
        action="store_true",
        help="skip the gdigrab-based legacy stdin test on a non-interactive desktop",
    )
    args = parser.parse_args()

    if os.name != "nt":
        parser.error("this integration test requires Windows")

    ffmpeg = os.path.abspath(args.ffmpeg)
    test_without_control_channel(ffmpeg)
    test_invalid_control_handle(ffmpeg)
    if not args.skip_legacy:
        test_legacy_stdin_control(ffmpeg)
    test_dedicated_control_handle(ffmpeg)
    print("runtime control integration tests passed")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"runtime control integration test failed: {error}", file=sys.stderr)
        raise
