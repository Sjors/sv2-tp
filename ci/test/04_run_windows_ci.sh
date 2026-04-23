#!/usr/bin/env bash
#
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.

export LC_ALL=C

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly REPO_ROOT
cd "${REPO_ROOT}"

# Keep native Windows paths and command-line options unchanged under MSYS.
export MSYS_NO_PATHCONV=1

case "${1:-}" in
    validate-manifest)
        # PowerShell expands the SDK variables, not Bash.
        # shellcheck disable=SC2016
        mt_exe=$(powershell.exe -NoProfile -Command '
            $ErrorActionPreference = "Stop"
            $sdk_dir = (Get-ItemProperty "HKLM:\SOFTWARE\Wow6432Node\Microsoft\Windows Kits\Installed Roots" -Name KitsRoot10).KitsRoot10
            $sdk_latest = (Get-ChildItem "$sdk_dir\bin" -Directory | Where-Object { $_.Name -match "^\d+\.\d+\.\d+\.\d+$" } | Sort-Object Name -Descending | Select-Object -First 1).Name
            Write-Output "${sdk_dir}bin\${sdk_latest}\x64\mt.exe"
        ')
        mt_exe=$(cygpath -u "${mt_exe//$'\r'/}")
        "${mt_exe}" -nologo '-inputresource:bin\sv2-tp.exe' -out:sv2-tp.manifest
        cat sv2-tp.manifest
        "${mt_exe}" -nologo '-inputresource:bin\sv2-tp.exe' -validate_manifest
        ;;
    *)
        echo "Usage: $0 validate-manifest" >&2
        exit 1
        ;;
esac
