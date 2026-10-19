"""Setup codes and owners, from the relay host.

Run on the relay host (no admin token needed; it uses the relay's own
database, so only someone with shell access can use it):

    docker compose exec relay python -m ostrelay.setup_code             # single-use code
    docker compose exec relay python -m ostrelay.setup_code --invite NAME
    docker compose exec relay python -m ostrelay.setup_code --rotate NAME
    docker compose exec relay python -m ostrelay.setup_code --revoke NAME
    docker compose exec relay python -m ostrelay.setup_code --list

The owner pastes the printed code into the timer page (Settings -> Remote
sharing). A plain setup code works once, within 15 minutes; an owner's code
is reusable until it is rotated or revoked (OSTRELAY_ENROLLMENT must include
``owner_codes``).
"""

from __future__ import annotations

import argparse
import time

import asyncio

from . import db, owners, tlsid
from .admin import create_enrollment_code, setup_code_for
from .config import settings


def _print_owner_code(name: str, code: str) -> None:
    print(f"Setup code for {name} (reusable for each of their timers; shown only now):\n")
    print(setup_code_for(code) or code)
    if "owner_codes" not in settings().enrollment_modes():
        print("\nNote: OSTRELAY_ENROLLMENT does not include owner_codes, so this code "
              "is refused until it does.")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m ostrelay.setup_code")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--invite", metavar="NAME", help="add an owner and print their reusable code")
    action.add_argument("--rotate", metavar="NAME", help="replace an owner's code (their timers stay)")
    action.add_argument("--revoke", metavar="NAME", help="stop an owner's code and remove their timers")
    action.add_argument("--list", action="store_true", help="list owners")
    args = parser.parse_args(argv)
    # This command is its own process: read the served TLS key once so the
    # setup code can carry its pin.
    asyncio.run(tlsid.refresh(force=True))
    db.connect()  # applies the schema on a fresh volume
    try:
        if args.invite:
            _print_owner_code(args.invite, owners.create_owner(args.invite))
        elif args.rotate:
            _print_owner_code(args.rotate, owners.rotate_owner(args.rotate))
            print("\nThe previous code no longer works.")
        elif args.revoke:
            removed = owners.revoke_owner(args.revoke)
            print(f"Revoked {args.revoke}; removed {len(removed)} timer(s). "
                  "Connected timers are disconnected within a minute.")
        elif args.list:
            rows = owners.list_owners()
            for row in rows:
                state = "revoked" if row["revoked"] else f"{row['timers']} timer(s)"
                print(f"{row['name']:<40} {state}")
            if not rows:
                print("No owners.")
        else:
            minted = create_enrollment_code()
            remaining = max(0, minted.expires_at_unix - int(time.time()))
            if minted.setup_code:
                print("Setup code for the timer page (Settings -> Remote sharing):\n")
                print(minted.setup_code)
            else:
                print("The relay could not read the TLS key served at its public URL, so no")
                print("setup code can be built. Enrollment code (enter it with the relay")
                print("address and key pin manually):\n")
                print(minted.enrollment_code)
            print(f"\nSingle use; expires in {remaining // 60} minutes.")
    except ValueError as error:
        print(f"error: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
