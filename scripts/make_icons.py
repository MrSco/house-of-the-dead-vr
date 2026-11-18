#!/usr/bin/env python3
"""The app icon on every platform, and the README's banner, from the artwork in assets/branding.

  python scripts/make_icons.py

assets/branding/app-icon.png (square) becomes:
  src/quest/res/mipmap-*/ic_launcher.png        the APKs' (Quest, Steam Frame, phones; quest_tools.py package_apk)
  src/flat/win/hotdvr.ico                         HOTDVR.exe's (src/flat/win/hotdvr.rc)
  src/flat/mac/AppIcon.icns                       the Mac app's (cmake/flat_apple.cmake)
  src/flat/ios/Assets.xcassets/AppIcon.appiconset the iPhone and iPad app's (src/flat/ios/project.yml)
assets/branding/banner.png becomes docs/images/header.jpg (the README's top). Run it again when the artwork changes,
and commit what it writes. Needs Pillow.
"""
import json
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRAND = os.path.join(ROOT, 'assets', 'branding')


def out(*p):
    path = os.path.join(ROOT, *p)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


def main():
    icon = Image.open(os.path.join(BRAND, 'app-icon.png')).convert('RGB')
    assert icon.width == icon.height, 'the app icon must be square'
    sized = lambda n: icon.resize((n, n), Image.LANCZOS)
    # Android: the launcher icon at each density (48 dp)
    for d, n in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
        sized(n).save(out('src', 'quest', 'res', 'mipmap-' + d, 'ic_launcher.png'), optimize=True)
    # Windows: one .ico with the sizes Explorer and the taskbar ask for
    sized(256).save(out('src', 'flat', 'win', 'hotdvr.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    # Mac
    sized(1024).save(out('src', 'flat', 'mac', 'AppIcon.icns'))
    # iOS: one 1024 icon (Xcode 14 and later make the rest), no alpha
    cat = out('src', 'flat', 'ios', 'Assets.xcassets', 'AppIcon.appiconset', 'Contents.json')
    sized(1024).save(os.path.join(os.path.dirname(cat), 'AppIcon-1024.png'), optimize=True)
    json.dump({'images': [{'filename': 'AppIcon-1024.png', 'idiom': 'universal', 'platform': 'ios', 'size': '1024x1024'}],
               'info': {'author': 'xcode', 'version': 1}}, open(cat, 'w'), indent=2)
    json.dump({'info': {'author': 'xcode', 'version': 1}},
              open(out('src', 'flat', 'ios', 'Assets.xcassets', 'Contents.json'), 'w'), indent=2)
    # the README's banner
    banner = Image.open(os.path.join(BRAND, 'banner.png')).convert('RGB')
    if banner.width > 1600:
        banner = banner.resize((1600, round(banner.height * 1600 / banner.width)), Image.LANCZOS)
    banner.save(out('docs', 'images', 'header.jpg'), quality=88, optimize=True, progressive=True)
    print('icons and banner written')


if __name__ == '__main__':
    main()
