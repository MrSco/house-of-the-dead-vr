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
  src/visionos/Assets.xcassets                    Vision Pro's layered round icon (src/visionos/project.yml);
                                                  only these: python scripts/make_icons.py visionos
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
    visionos_icon()
    # the README's banner
    banner = Image.open(os.path.join(BRAND, 'banner.png')).convert('RGB')
    if banner.width > 1600:
        banner = banner.resize((1600, round(banner.height * 1600 / banner.width)), Image.LANCZOS)
    banner.save(out('docs', 'images', 'header.jpg'), quality=88, optimize=True, progressive=True)
    print('icons and banner written')


# Apple TV: the home screen's icon is a wide (5:3) layered stack that tilts under the focus: the banner, filling it,
# behind a clear front layer (tvOS asks for two layers or more). The Top Shelf (shown while the icon has the focus) is
# the banner too. Every size App Store validation asks for, at 1x and 2x; the opaque pictures as JPEG (the repository stays
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


def tv_back(banner, w, h):
    return tv_cover(banner, w, h)


def tv_front(banner, w, h):
    return Image.new('RGBA', (w, h), (0, 0, 0, 0))


def tvos_brand_assets():
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
                tv_save(draw(banner, w * k, h * k), os.path.join(root, layer + '.imagestacklayer', 'Content.imageset', name))
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


# Vision Pro: the home view's icon is a round stack of up to three 1024 layers (a solid image stack; App Store
# validation refuses the app without one). The square icon is the opaque back layer (visionOS masks it to a circle),
# the middle and front layers clear. As GoldenEye VR's port/visionos/Assets.xcassets.
def visionos_icon():
    icon = Image.open(os.path.join(BRAND, 'app-icon.png')).convert('RGB').resize((1024, 1024), Image.LANCZOS)
    base = os.path.join(ROOT, 'src', 'visionos', 'Assets.xcassets')
    stack = os.path.join(base, 'AppIcon.solidimagestack')
    shutil.rmtree(stack, ignore_errors=True)
    tv_json(os.path.join(base, 'Contents.json'), {'info': TV_INFO})
    layers = (('Front', 'front.png', None), ('Middle', 'middle.png', None), ('Back', 'back.jpg', icon))
    for layer, name, img in layers:
        d = os.path.join(stack, layer + '.solidimagestacklayer')
        tv_save(img if img else Image.new('RGBA', (1024, 1024), (0, 0, 0, 0)), os.path.join(d, 'Content.imageset', name))
        tv_json(os.path.join(d, 'Content.imageset', 'Contents.json'),
                {'images': [{'filename': name, 'idiom': 'vision', 'scale': '2x'}], 'info': TV_INFO})
        tv_json(os.path.join(d, 'Contents.json'), {'info': TV_INFO})
    tv_json(os.path.join(stack, 'Contents.json'), {'info': TV_INFO, 'layers': [{'filename': l[0] + '.solidimagestacklayer'} for l in layers]})
    print('Vision Pro icon written')


if __name__ == '__main__':
    if sys.argv[1:] == ['tvos']: tvos_brand_assets()
    elif sys.argv[1:] == ['visionos']: visionos_icon()
    else: main()
