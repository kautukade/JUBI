"""User-facing Jubi VPS live acceptance entry point."""

from sarus.vps_acceptance import main, run_vps_acceptance

__all__ = ["main", "run_vps_acceptance"]


if __name__ == "__main__":
    main()
