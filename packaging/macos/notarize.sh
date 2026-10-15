#!/usr/bin/env bash
# Signs and notarizes a LanAudio DMG when Apple credentials are present.
# The pipeline is wired so a missing secret fails closed instead of shipping
# an unsigned production artifact as if it were notarized.
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: notarize.sh <dmg> <arch>" >&2
  exit 2
fi

dmg=$1
arch=$2
identity=${MACOS_SIGNING_IDENTITY:-}
profile=${MACOS_NOTARY_PROFILE:-}

if [[ -z "${identity}" || -z "${profile}" ]]; then
  echo "Apple signing secrets are not configured; leaving ${dmg} unsigned." >&2
  echo "Set MACOS_SIGNING_IDENTITY and MACOS_NOTARY_PROFILE for production." >&2
  if [[ "${LANAUDIO_REQUIRE_NOTARIZATION:-}" == "1" ]]; then
    echo "LANAUDIO_REQUIRE_NOTARIZATION=1 but credentials are missing." >&2
    exit 1
  fi
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "${tmp}"' EXIT
hdiutil attach -nobrowse -readonly -mountpoint "${tmp}/mnt" "${dmg}"
app="${tmp}/mnt/LanAudio.app"
if [[ ! -d "${app}" ]]; then
  echo "LanAudio.app is missing from ${dmg}" >&2
  exit 1
fi

codesign --force --options runtime --timestamp --sign "${identity}" \
  --entitlements "$(dirname "$0")/entitlements.plist" "${app}"
codesign --verify --deep --strict "${app}"
hdiutil detach "${tmp}/mnt"

xcrun notarytool submit "${dmg}" --keychain-profile "${profile}" --wait
xcrun stapler staple "${dmg}"
echo "notarized ${dmg} (${arch})"
