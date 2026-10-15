#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <LanAudio.dmg> <arm64|x86_64>" >&2
  exit 2
fi

dmg=$1
expected_arch=$2
if [[ ! -f "$dmg" ]]; then
  echo "DMG does not exist: $dmg" >&2
  exit 1
fi
case "$expected_arch" in
  arm64|x86_64) ;;
  *)
    echo "unsupported expected architecture: $expected_arch" >&2
    exit 2
    ;;
esac

work_dir=$(mktemp -d)
mount_point="$work_dir/mount"
mkdir -p "$mount_point"
mounted=false
cleanup() {
  if [[ "$mounted" == true ]]; then
    hdiutil detach "$mount_point" -quiet ||
      hdiutil detach "$mount_point" -force -quiet || true
  fi
  if ! mount | grep -Fq " on $mount_point "; then
    rm -rf "$work_dir"
  fi
}
trap cleanup EXIT

hdiutil attach "$dmg" -nobrowse -readonly -mountpoint "$mount_point" -quiet
mounted=true

app="$mount_point/LanAudio.app"
main_executable="$app/Contents/MacOS/LanAudio"
cli_executable="$app/Contents/MacOS/lanaudio-cli"
plist="$app/Contents/Info.plist"

for required in "$app" "$main_executable" "$cli_executable" "$plist"; do
  if [[ ! -e "$required" ]]; then
    echo "required package entry is missing: $required" >&2
    exit 1
  fi
done

verify_contains_arch() {
  local binary=$1
  local architectures
  /usr/bin/lipo -info "$binary" >/dev/null
  architectures=$(/usr/bin/lipo -archs "$binary")
  if [[ " $architectures " != *" $expected_arch "* ]]; then
    echo "$binary does not contain $expected_arch (has: $architectures)" >&2
    exit 1
  fi
}

for executable in "$main_executable" "$cli_executable"; do
  file "$executable"
  /usr/bin/lipo -info "$executable"
  architectures=$(/usr/bin/lipo -archs "$executable")
  if [[ "$architectures" != "$expected_arch" ]]; then
    echo "architecture-specific executable is not thin $expected_arch: " \
      "$executable has $architectures" >&2
    exit 1
  fi
done

while IFS= read -r -d '' candidate; do
  if file -b "$candidate" | grep -q "Mach-O"; then
    verify_contains_arch "$candidate"
    if otool -L "$candidate" | grep -E \
      '(/opt/homebrew|/usr/local/(Cellar|opt)|/Users/|/build[^/]*[/])'; then
      echo "non-system development dependency remains in $candidate" >&2
      exit 1
    fi
    while IFS= read -r dependency; do
      dependency=$(awk '{print $1}' <<<"$dependency")
      case "$dependency" in
        @rpath/*)
          relative=${dependency#@rpath/}
          if [[ ! -e "$app/Contents/Frameworks/$relative" ]]; then
            echo "unresolved bundled dependency in $candidate: $dependency" >&2
            exit 1
          fi
          ;;
        /System/*|/usr/lib/*|@loader_path/*|@executable_path/*) ;;
        /*)
          echo "unexpected absolute dependency in $candidate: $dependency" >&2
          exit 1
          ;;
      esac
    done < <(
      # Universal binaries add per-architecture heading lines. Only indented
      # load-command entries beginning with a path or dyld token are deps.
      otool -L "$candidate" | awk '/^[[:space:]]+(@|\/)/ { print }'
    )
  fi
done < <(find "$app" -type f -print0)

/usr/bin/codesign --verify --deep --strict "$app"

for key in CFBundleDisplayName CFBundleIdentifier CFBundleShortVersionString \
           CFBundleVersion LSMinimumSystemVersion \
           NSMicrophoneUsageDescription NSLocalNetworkUsageDescription \
           LanAudioBuildArchitecture; do
  if ! /usr/libexec/PlistBuddy -c "Print :$key" "$plist" >/dev/null; then
    echo "Info.plist is missing $key" >&2
    exit 1
  fi
done
plist_arch=$(
  /usr/libexec/PlistBuddy -c "Print :LanAudioBuildArchitecture" "$plist"
)
if [[ "$plist_arch" != "$expected_arch" ]]; then
  echo "Info.plist architecture is $plist_arch, expected $expected_arch" >&2
  exit 1
fi

diagnostics=$("$cli_executable" diagnostics)
echo "$diagnostics"
if ! grep -q "AEAD_AES_128_GCM" <<<"$diagnostics"; then
  echo "packaged CLI is not reporting SRTP AEAD_AES_128_GCM protection" >&2
  exit 1
fi
if grep -Eqi "not-implemented|unencrypted" <<<"$diagnostics"; then
  echo "packaged CLI still reports unauthenticated media" >&2
  exit 1
fi
"$main_executable" --smoke-test

echo "Verified $dmg for $expected_arch"
