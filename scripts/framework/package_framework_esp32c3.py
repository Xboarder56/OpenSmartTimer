#!/usr/bin/env python3
"""Write a byte-reproducible gzip-compressed framework tar archive."""

from __future__ import annotations

import argparse
import gzip
import os
import tarfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--mtime", required=True, type=int)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    paths = [args.package]
    paths.extend(sorted(args.package.rglob("*"), key=lambda p: p.as_posix()))
    with args.output.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw,
                           mtime=args.mtime, compresslevel=9) as zipped:
            with tarfile.open(fileobj=zipped, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for path in paths:
                    relative = path.relative_to(args.package.parent)
                    info = archive.gettarinfo(str(path), arcname=relative.as_posix())
                    info.uid = 0
                    info.gid = 0
                    info.uname = "root"
                    info.gname = "root"
                    info.mtime = args.mtime
                    if info.isfile():
                        with path.open("rb") as stream:
                            archive.addfile(info, stream)
                    else:
                        archive.addfile(info)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
