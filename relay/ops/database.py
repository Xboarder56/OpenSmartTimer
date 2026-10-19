#!/usr/bin/env python3
"""SQLite online backup and offline restore to a NEW destination file.

Never copy a live WAL database with cp. Restore requires the relay stopped;
this utility deliberately refuses to overwrite any destination.
"""
import argparse
import os
import sqlite3
from pathlib import Path


def copy_database(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise ValueError("source database does not exist")
    # Exclusive creation protects an existing operator database/snapshot.
    fd = os.open(destination, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)
    try:
        with sqlite3.connect(source.resolve().as_uri() + "?mode=ro", uri=True) as src:
            with sqlite3.connect(destination) as dst:
                src.backup(dst)
                if dst.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
                    raise ValueError("database integrity check failed")
        os.chmod(destination, 0o600)
    except BaseException:
        destination.unlink(missing_ok=True)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["backup", "restore"])
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    copy_database(args.source, args.destination)
    print(f"{args.action}: integrity check passed")
