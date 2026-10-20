#!/usr/bin/env python3
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# Licensed under the CANN Open Software License Agreement Version 2.0.
"""Prepare a private CANN 9.1 view; never write through links into the installation."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import struct
import tarfile

AICPU = Path("opp/built-in/op_impl/aicpu")


def read_candidate(archive):
    with tarfile.open(archive, "r:gz") as package:
        entries = {}
        for name in ("libccl_kernel.so", "libccl_kernel_plf.so", "bin_hash.cfg"):
            member = package.getmember("aicpu_kernels_device/" + name)
            if not member.isfile():
                raise ValueError("candidate is not a regular file: " + name)
            entries[name] = package.extractfile(member).read()
    hashes = dict(line.split("=", 1) for line in entries["bin_hash.cfg"].decode().splitlines() if line)
    for name in ("libccl_kernel.so", "libccl_kernel_plf.so"):
        data = entries[name]
        if hashlib.sha256(data).hexdigest() != hashes.get(name):
            raise ValueError("candidate manifest mismatch: " + name)
        if len(data) < 20 or data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
            raise ValueError("candidate must be a little-endian AArch64 ELF64: " + name)
    if b"invalid local buffer DTO layout" not in entries["libccl_kernel.so"]:
        raise ValueError("candidate libccl_kernel.so does not contain the parser fix marker")
    return entries


def create_view(source, destination, replacements, relative=Path()):
    destination.mkdir()
    for entry in source.iterdir():
        child = relative / entry.name
        if child in replacements:
            continue
        if any(child in path.parents for path in replacements):
            create_view(entry, destination / entry.name, replacements, child)
        else:
            (destination / entry.name).symlink_to(entry, target_is_directory=entry.is_dir())


def stage(cann, archive, kernel_json, overlay):
    version_file = cann / "share/info/hcomm/version.info"
    version = version_file.read_text()
    if not re.search(r"(?m)^Version=9\.1\.0\s*$", version):
        raise ValueError("expected the server's HCOMM 9.1.0 installation: " + str(version_file))
    if overlay == cann or cann in overlay.parents or overlay in cann.parents:
        raise ValueError("overlay and installed CANN must be separate directory trees")
    entries = read_candidate(archive)
    config = json.loads(kernel_json.read_text())
    if config.get("RunAicpuChannelInitV3", {}).get("opInfo", {}).get("kernelSo") != "libccl_kernel.so":
        raise ValueError("candidate JSON does not describe the expected HCOMM AICPU entry")
    replacements = {
        AICPU / "config/ccl_kernel.json": kernel_json.read_bytes(),
        AICPU / "kernel/aicpu_hcomm.tar.gz": archive.read_bytes(),
    }
    device_dirs = list(cann.glob("*-linux/devlib/device"))
    if not device_dirs:
        raise ValueError("installed CANN is missing *-linux/devlib/device")
    for device_dir in device_dirs:
        for name in ("libccl_kernel.so", "libccl_kernel_plf.so"):
            replacements[device_dir.relative_to(cann) / name] = entries[name]
    # Existing directories, including links to installations, are deliberately rejected.
    if overlay.exists() or overlay.is_symlink():
        raise FileExistsError("use a new overlay directory: " + str(overlay))
    create_view(cann, overlay, replacements)
    for relative, data in replacements.items():
        (overlay / relative).write_bytes(data)
    manifest = {"installed_cann": str(cann), "candidate_archive": str(archive),
                "files": {str(path): hashlib.sha256(data).hexdigest() for path, data in replacements.items()}}
    (overlay / "local_mr_dto_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    view = shlex.quote(str(overlay))
    (overlay / "local_mr_dto_env.sh").write_text(
        "# Source this AFTER the normal CANN and existing MF overlay environment.\n"
        f"export ASCEND_HOME_PATH={view}\n"
        f"export ASCEND_AICPU_PATH={view}\n"
        f"export ASCEND_OPP_PATH={view}/opp\n")
    print("Prepared private candidate:", overlay)
    print("Fresh processes must verify candidate JSON/package loading and fallback_count=0 on the server.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cann", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True, help="build/device_build/aicpu_hcomm.tar.gz")
    parser.add_argument("--kernel-json", type=Path, required=True, help="generated ccl_kernel.json from the same build")
    parser.add_argument("--overlay", type=Path, required=True, help="new private directory, outside installed CANN")
    args = parser.parse_args()
    stage(*(path.resolve() for path in (args.cann, args.archive, args.kernel_json, args.overlay)))


if __name__ == "__main__":
    main()
