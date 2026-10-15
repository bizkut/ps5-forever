#!/usr/bin/env bash
# Shared checksum spelling for the macOS and Linux build hosts.
ps5_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    else
        shasum -a 256 "$@"
    fi
}
