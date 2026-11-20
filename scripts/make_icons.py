#!/usr/bin/env python3
"""The app icon on every platform, and the README's banner, from the artwork in assets/branding.

  python scripts/make_icons.py

assets/branding/app-icon.png (square) becomes:
  src/quest/res/mipmap-*/ic_launcher.png        the APKs' (Quest, Steam Frame, phones; quest_tools.py package_apk)
  src/flat/win/hotdvr.ico                         HOTDVR.exe's (src/flat/win/hotdvr.rc)
  src/flat/mac/AppIcon.icns                       the Mac app's (cmake/flat_apple.cmake)
  src/flat/ios/Assets.xcassets/AppIcon.appiconset the iPhone and iPad app's (src/flat/ios/project.yml)
  src/flat/tvos/Assets.xcassets                   Apple TV's layered icon and Top Shelf (src/flat/tvos/project.yml);
                                                  only these: python scripts/make_icons.py tvos
assets/branding/banner.png becomes docs/images/header.jpg (the README's top). Run it again when the artwork changes,
and commit what it writes. Needs Pillow.
"""
import json
import os
import shutil
import sys
from PIL import Image, ImageFilter

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
    tvos_brand_assets()
    # the README's banner
    banner = Image.open(os.path.join(BRAND, 'banner.png')).convert('RGB')
    if banner.width > 1600:
        banner = banner.resize((1600, round(banner.height * 1600 / banner.width)), Image.LANCZOS)
    banner.save(out('docs', 'images', 'header.jpg'), quality=88, optimize=True, progressive=True)
    print('icons and banner written')


# Apple TV: the home screen's icon is a layered stack that tilts under the focus: the app icon, whole, in front of a
# blurred, darker copy of it filling the TV's wider shape. The Top Shelf (shown while the icon has the focus) is the
# banner. Every size App Store validation asks for, at 1x and 2x; the opaque pictures as JPEG (the repository stays
# small). As GoldenEye VR's tools/make_icons.py tvos.
TV_INFO = {'author': 'xcode', 'version': 1}


def tv_json(path, doc):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', newline='\n') as f:
        json.dump(doc, f, indent=2)
        f.write('\n')


def tv_save(img, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if path.endswith('.jpg'): img.convert('RGB').save(path, quality=86, optimize=True, progressive=True)
    else: img.save(path, optimize=True)


def tv_cover(src, w, h):
    k = max(w / src.width, h / src.height)
    src = src.resize((int(src.width * k + 0.5), int(src.height * k + 0.5)), Image.LANCZOS)
    x0, y0 = (src.width - w) // 2, (src.height - h) // 2
    return src.crop((x0, y0, x0 + w, y0 + h))


def tv_back(icon, w, h):
    img = tv_cover(icon, w, h).filter(ImageFilter.GaussianBlur(w * 0.025))
    return Image.blend(img, Image.new('RGB', (w, h), (0, 0, 0)), 0.35)


def tv_front(icon, w, h):
    side = int(h * 0.88)
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    out.paste(icon.resize((side, side), Image.LANCZOS), ((w - side) // 2, (h - side) // 2))
    return out


def tvos_brand_assets():
    icon = Image.open(os.path.join(BRAND, 'app-icon.png')).convert('RGB')
    banner = Image.open(os.path.join(BRAND, 'banner.png')).convert('RGB')
    base = os.path.join(ROOT, 'src', 'flat', 'tvos', 'Assets.xcassets')
    brand = os.path.join(base, 'App Icon & Top Shelf Image.brandassets')
    shutil.rmtree(brand, ignore_errors=True)
    tv_json(os.path.join(base, 'Contents.json'), {'info': TV_INFO})
    for stack, (w, h), scales in (('App Icon.imagestack', (400, 240), (1, 2)),
                                  ('App Icon - App Store.imagestack', (1280, 768), (1,))):
        root = os.path.join(brand, stack)
        layers = (('Front', 'front', 'png', tv_front), ('Back', 'back', 'jpg', tv_back))
        for layer, stem, ext, draw in layers:
            images = []
            for k in scales:
                name = '%s%s.%s' % (stem, '' if k == 1 else '@%dx' % k, ext)
                tv_save(draw(icon, w * k, h * k), os.path.join(root, layer + '.imagestacklayer', 'Content.imageset', name))
                images.append({'filename': name, 'idiom': 'tv', 'scale': '%dx' % k})
            tv_json(os.path.join(root, layer + '.imagestacklayer', 'Content.imageset', 'Contents.json'), {'images': images, 'info': TV_INFO})
            tv_json(os.path.join(root, layer + '.imagestacklayer', 'Contents.json'), {'info': TV_INFO})
        tv_json(os.path.join(root, 'Contents.json'), {'info': TV_INFO, 'layers': [{'filename': l[0] + '.imagestacklayer'} for l in layers]})
    for folder, stem, (w, h) in (('Top Shelf Image.imageset', 'shelf', (1920, 720)),
                                 ('Top Shelf Image Wide.imageset', 'shelf-wide', (2320, 720))):
        images = []
        for k in (1, 2):
            name = '%s%s.jpg' % (stem, '' if k == 1 else '@2x')
            tv_save(tv_cover(banner, w * k, h * k), os.path.join(brand, folder, name))
            images.append({'filename': name, 'idiom': 'tv', 'scale': '%dx' % k})
        tv_json(os.path.join(brand, folder, 'Contents.json'), {'images': images, 'info': TV_INFO})
    tv_json(os.path.join(brand, 'Contents.json'), {'assets': [
        {'filename': 'App Icon - App Store.imagestack', 'idiom': 'tv', 'role': 'primary-app-icon', 'size': '1280x768'},
        {'filename': 'App Icon.imagestack', 'idiom': 'tv', 'role': 'primary-app-icon', 'size': '400x240'},
        {'filename': 'Top Shelf Image Wide.imageset', 'idiom': 'tv', 'role': 'top-shelf-image-wide', 'size': '2320x720'},
        {'filename': 'Top Shelf Image.imageset', 'idiom': 'tv', 'role': 'top-shelf-image', 'size': '1920x720'},
    ], 'info': TV_INFO})
    print('Apple TV icons written')


if __name__ == '__main__':
    if sys.argv[1:] == ['tvos']: tvos_brand_assets()
    else: main()
