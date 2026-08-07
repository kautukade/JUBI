# SARA bundle layout

`vendor/sara/finalparts/` + `vendor/sara/FINAL-SHA256.txt` is the only authoritative bundled SARA source format for SARUS.

The older `parts/`, `xzparts/`, and `xz2parts/` directories are historical/incomplete staging attempts and MUST NOT be used by the installer.

The installer must fail closed unless all 24 `finalparts/part-*.b64` files are present and the reconstructed `SARA-public-final.tar.xz` matches SHA256:

`695b9bd77d16f2049bdead078af886a2e2cf9aeff56543a4b5d180867b913ddb`

If the verified bundle is unavailable, the installer may use the owner's authenticated SARA GitHub source as a fallback. Public support-report archives, local diagnostics and files containing private machine/network information are intentionally excluded.
