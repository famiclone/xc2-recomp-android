"""Package XC2 Recomp assets and configuration for Android (arm64-v8a / 8 GB unified RAM).

Prepares a clean `build/xc2-data/` folder ready to push to `/sdcard/Android/data/com.xc2recomp.app/files/`:
  - `xc2.ini`         8 GB RAM survival defaults (720p ceiling, FSR 1 upscale, native ASTC, unified_vulkan)
  - `exefs/`          extracted Switch ExeFS (main.npdm, main, subsdk0, sdk)
  - `romfs/`          merged Switch 3.0.1 RomFS (`romfs.bin`) + `romfs-index.bin` + `romfs-patches/`
  - `prompts/`        controller button prompt overlays (when generated)
  - `shader-cache/`   pre-translated SPIR-V shader cache + `xc2_xlate.id`
  - `mods/`           empty mod directory

Also copies `build/<version>-android/libmain.so` (when built by `tools/build_game.py --target android-arm64`) into
`android/app/src/main/jniLibs/arm64-v8a/libmain.so` so Gradle packages it into the APK.

With `--selftest`, builds a standalone `libmain.so` without requiring Switch NSPs or `generated/` code so the Android
APK and runtime can be verified before game extraction.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
import build_game
import romfs_index

ANDROID_XC2_INI = """\
# XC2 Recomp — Android 8 GB Unified RAM Profile (Snapdragon 8 Gen 2 / Adreno 740)
# Push this folder to: /sdcard/Android/data/com.xc2recomp.app/files/

[display]
fullscreen = 1
window_size = 1920x1080
output_res = 1280x720
render_scale = 75
upscaler = fsr
sharpness = 80
picture_fit = keep
fsr3 = 0
frame_gen = 0
vk_present = fifo
perf_overlay = fps

[frame rate]
fps_limit = 60
fps60 = 1
cutscene_30 = 1
realtime = 1
cloth_realtime = 1
highfps_fix = 1

[language]
operation_mode = handheld

[input]
pad_layout = auto
prompts = auto
motion = 1
rumble_strength = 100

[performance]
memory = unified_vulkan
ring_stage_mb = 128
astc = native
vram_headroom = 0
tex_alias = 1
zstd_native = 1
replay_async = 1
async_preempt = 1
android_pin = 1
pipe_prebuild = 1
lib_prebuild = 1
lib_prebuild_threads = 2
# Custom Mesa Turnip driver bundled in nativeLibraryDir (libvulkan_turnip.so via libadrenotools); comment out for stock Adreno:
vk_driver = turnip
"""


def link_or_copy(src: Path, dst: Path, hardlink: bool = False):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.unlink()
    if hardlink:
        try:
            os.link(src, dst)
            return
        except OSError:
            pass
    shutil.copy2(src, dst)


def sync_tree(src: Path, dst: Path, ignore_names=(), hardlink: bool = False):
    if not src.is_dir():
        return 0
    skip = set(ignore_names) | {'.DS_Store'}
    count = 0
    for root, dirs, files in os.walk(src):
        rel = Path(root).relative_to(src)
        target_dir = dst / rel
        target_dir.mkdir(parents=True, exist_ok=True)
        dirs[:] = [d for d in dirs if d not in skip]
        for f in files:
            if f in skip or f.startswith('._'):
                continue
            sp = Path(root) / f
            dp = target_dir / f
            if not dp.is_file() or dp.stat().st_size != sp.stat().st_size or int(dp.stat().st_mtime) < int(sp.stat().st_mtime):
                link_or_copy(sp, dp, hardlink=hardlink)
            count += 1
    return count


def build_selftest_so(ndk_bin: Path, out_dir: Path, deps_dir: Path | None):
    """Compile runtime/ + selftest_modules.c into build/android-selftest/libxc2rt_full.a (and libmain.so when deps exist)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    romfs_index.write_placeholder(out_dir)
    (out_dir / 'patches.h').write_text(
        '// Empty patch header for Android selftest build.\n'
        '#pragma once\n'
        '#define RT_PATCH_COUNT 0\n',
        encoding='utf-8',
    )
    clang = ndk_bin / ('aarch64-linux-android29-clang.cmd' if os.name == 'nt' else 'aarch64-linux-android29-clang')
    clangxx = ndk_bin / ('aarch64-linux-android29-clang++.cmd' if os.name == 'nt' else 'aarch64-linux-android29-clang++')
    ar = ndk_bin / ('llvm-ar.exe' if os.name == 'nt' else 'llvm-ar')

    build_game.ANDROID_TARGET = True
    sources = build_game.runtime_sources() + [ROOT / 'android' / 'app' / 'src' / 'main' / 'cpp' / 'selftest_modules.c']
    vma_inc = build_game.vma_include()
    inc_dirs = [
        ROOT / 'runtime',
        ROOT / 'runtime' / 'third_party' / 'imgui',
        vma_inc,
        out_dir,
    ]
    if deps_dir and (deps_dir / 'include').is_dir():
        inc_dirs.append(deps_dir / 'include')
    else:
        for brew in ('/opt/homebrew/opt/sdl3/include', '/opt/homebrew/opt/opus/include',
                     '/opt/homebrew/opt/libvpx/include', '/opt/homebrew/opt/zstd/include'):
            if Path(brew).is_dir():
                inc_dirs.append(Path(brew))

    base_flags = ['-O2', '-g', '-fPIC', '-Wall', '-fno-strict-aliasing',
                  '-march=armv8.2-a+fp16+dotprod', '-DVK_NO_PROTOTYPES']
    for d in inc_dirs:
        base_flags += ['-isystem' if str(d).startswith('/opt/homebrew') else '-I', str(d)]

    obj_root = out_dir / 'obj'

    def compile_one(src: Path):
        rel = src.relative_to(ROOT) if src.is_relative_to(ROOT) else Path(src.name)
        obj = obj_root / rel.with_suffix('.o')
        obj.parent.mkdir(parents=True, exist_ok=True)
        if obj.is_file() and obj.stat().st_mtime >= src.stat().st_mtime:
            return obj
        is_cxx = src.suffix == '.cpp'
        cmd = [str(clangxx if is_cxx else clang)] + (['-std=c++20', '-fno-exceptions', '-fno-rtti'] if is_cxx else ['-std=gnu11'])
        cmd += base_flags + ['-c', str(src), '-o', str(obj)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f'compile failed for {src}:\n{r.stderr}')
        return obj

    with ThreadPoolExecutor(min(8, os.cpu_count() or 4)) as pool:
        objs = list(pool.map(compile_one, sources))

    archive = out_dir / 'libxc2rt_selftest.a'
    archive.unlink(missing_ok=True)
    subprocess.run([str(ar), 'rcs', str(archive)] + [str(o) for o in objs], check=True)
    print(f'package_android: compiled {len(objs)} objects -> {archive.relative_to(ROOT)} '
          f'({archive.stat().st_size // 1024} KiB)')
    return archive, objs


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--version', default='v301', help='game version tag (default v301)')
    ap.add_argument('--build-dir', type=Path, help='Android game build directory (default build/<version>-android)')
    ap.add_argument('--extracted', type=Path, help='extracted game directory (default build/extracted/<version>)')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'xc2-data',
                    help='output folder to push to Android storage (default build/xc2-data)')
    ap.add_argument('--toolchain', type=Path, help='Android NDK directory override')
    ap.add_argument('--deps', type=Path, help='prebuilt Android arm64 deps directory (include/, lib/)')
    ap.add_argument('--turnip', type=Path, help='custom Turnip ICD .so to bundle as libvulkan_turnip.so')
    ap.add_argument('--apk-only', action='store_true',
                    help='only stage APK jniLibs and assets/xlate without re-syncing build/xc2-data/')
    ap.add_argument('--selftest', action='store_true',
                    help='compile runtime + selftest module glue without requiring extracted Switch NSPs')
    args = ap.parse_args()

    bdir = (args.build_dir or ROOT / 'build' / f'{args.version}-android').resolve()
    ext = (args.extracted or ROOT / 'build' / 'extracted' / args.version).resolve()
    out = args.out.resolve()

    out.mkdir(parents=True, exist_ok=True)
    ini_path = out / 'xc2.ini'
    ini_path.write_bytes(ANDROID_XC2_INI.encode('utf-8'))
    print(f'package_android: wrote 8 GB Android profile -> {ini_path.relative_to(ROOT)}')

    if not args.apk_only:
        (out / 'mods').mkdir(exist_ok=True)
        (out / 'logs').mkdir(exist_ok=True)
        (out / 'profile').mkdir(exist_ok=True)

        if args.selftest:
            ndk_bin = build_game.find_android_ndk_bin(args.toolchain)
            if not ndk_bin:
                sys.exit('package_android: Android NDK not found; set ANDROID_NDK_HOME or pass --toolchain')
            build_selftest_so(ndk_bin, ROOT / 'build' / 'android-selftest', args.deps.resolve() if args.deps else None)
            print(f'package_android: selftest stage ready at {out.relative_to(ROOT)}')
            return

        exefs = ext / 'exefs'
        if exefs.is_dir():
            n = sync_tree(exefs, out / 'exefs')
            print(f'package_android: synced exefs ({n} files)')
        else:
            print(f'package_android: note: {exefs} missing (extract Switch NSPs first)', file=sys.stderr)

        romfs = ext / 'romfs'
        if (romfs / 'romfs.bin').is_file():
            (out / 'romfs').mkdir(exist_ok=True)
            link_or_copy(romfs / 'romfs.bin', out / 'romfs' / 'romfs.bin', hardlink=True)
            print('package_android: linked romfs/romfs.bin')

        aoc = ext / 'aoc'
        if aoc.is_dir():
            n = sync_tree(aoc, out / 'aoc', ignore_names=('index.json',), hardlink=True)
            print(f'package_android: synced aoc/ ({n} DLC files)')

        for item in ('romfs-index.bin',):
            if (bdir / item).is_file():
                link_or_copy(bdir / item, out / item)
                print(f'package_android: copied {item}')

        for sub in ('romfs-patches', 'prompts', 'shader-cache'):
            src = bdir / sub if (bdir / sub).is_dir() else (ROOT / 'build' / args.version / sub)
            if src.is_dir():
                n = sync_tree(src, out / sub)
                print(f'package_android: synced {sub}/ ({n} files)')

        xlate_id = ROOT / 'build' / 'xlate' / 'xc2_xlate.id'
        if xlate_id.is_file():
            link_or_copy(xlate_id, out / 'xc2_xlate.id')
            print('package_android: copied xc2_xlate.id for pre-translated shader cache lookup')

    so_src = bdir / 'libmain.so'
    jni_dir = ROOT / 'android' / 'app' / 'src' / 'main' / 'jniLibs' / 'arm64-v8a'
    if so_src.is_file():
        jni_dst = jni_dir / 'libmain.so'
        link_or_copy(so_src, jni_dst)
        print(f'package_android: staged {jni_dst.relative_to(ROOT)}')
    deps_dir = args.deps.resolve() if args.deps else (ROOT / 'toolchain' / 'deps-android')
    for dep_so_name in ('libSDL3.so', 'libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so',
                        'libfile_redirect_hook.so', 'libgsl_alloc_hook.so'):
        dep_so = deps_dir / 'lib' / dep_so_name
        if dep_so.is_file():
            link_or_copy(dep_so, jni_dir / dep_so_name)
            print(f'package_android: staged {(jni_dir / dep_so_name).relative_to(ROOT)}')
    turnip_cands = ([args.turnip.resolve()] if args.turnip else []) + [
        ROOT / 'build' / 'turnip-t30' / 'vulkan.purple.so',
        deps_dir / 'lib' / 'libvulkan_turnip.so',
    ]
    for tc in turnip_cands:
        if tc.is_file():
            link_or_copy(tc, jni_dir / 'libvulkan_turnip.so')
            print(f'package_android: staged Turnip ICD {tc.name} -> {(jni_dir / "libvulkan_turnip.so").relative_to(ROOT)}')
            break
    xlate_so = ROOT / 'build' / 'xlate' / 'libxc2_xlate.so'
    if xlate_so.is_file():
        link_or_copy(xlate_so, jni_dir / 'libxc2_xlate.so')
        print(f'package_android: staged {(jni_dir / "libxc2_xlate.so").relative_to(ROOT)}')
    assets_dir = ROOT / 'android' / 'app' / 'src' / 'main' / 'assets'
    xlate_id = ROOT / 'build' / 'xlate' / 'xc2_xlate.id'
    if xlate_id.is_file():
        link_or_copy(xlate_id, assets_dir / 'xc2_xlate.id')
        if out.is_dir():
            link_or_copy(xlate_id, out / 'xc2_xlate.id')

    print(f'\nPush to device with:\n  adb push {out} /sdcard/Android/data/com.xc2recomp.app/files')


if __name__ == '__main__':
    main()
