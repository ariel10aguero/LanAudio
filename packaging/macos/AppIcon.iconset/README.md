# LanAudio application icon

Replace this file with the standard macOS iconset PNG files, then generate:

```bash
iconutil -c icns packaging/macos/AppIcon.iconset \
  -o packaging/macos/LanAudio.icns
```

When `packaging/macos/LanAudio.icns` exists, add it to the desktop target
resources and set `MACOSX_BUNDLE_ICON_FILE` to `LanAudio.icns`.

No placeholder icon is declared in `Info.plist`; macOS uses its generic
application icon until a real LanAudio asset is supplied.
