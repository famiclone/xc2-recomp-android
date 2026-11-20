"""Fetch and cross-compile Android arm64-v8a dependencies into toolchain/deps-android/.

Builds:
  - SDL3 (release-3.2.8) -> lib/libSDL3.so, include/SDL3/*, sdl-java/org/libsdl/app/*.java
  - libopus (v1.5.2)     -> lib/libopus.a, include/opus/*
  - zstd (v1.5.7)        -> lib/libzstd.a, include/zstd.h
  - libvpx (v1.15.0)     -> lib/libvpx.a, include/vpx/* (ARM64 NEON VP9 decoder)

Outputs land in git-ignored `toolchain/deps-android/`, automatically picked up by both
`tools/build_game.py --target android-arm64` and `android/app/build.gradle`.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import build_game  # noqa: E402


def find_ndk_root(hint: Path | None = None) -> Path:
    ndk_bin = build_game.find_android_ndk_bin(hint)
    if not ndk_bin:
        sys.exit('setup_android_deps: Android NDK not found; install android-ndk or set ANDROID_NDK_HOME')
    # <NDK>/toolchains/llvm/prebuilt/<host>/bin -> <NDK>
    return ndk_bin.parents[4]


def find_cmake() -> str:
    for c in (shutil.which('cmake'), '/opt/homebrew/bin/cmake', '/usr/local/bin/cmake'):
        if c and Path(c).is_file():
            return c
    sys.exit('setup_android_deps: cmake not found; install cmake (e.g. brew install cmake)')


def run(cmd, cwd=None, env=None):
    print('+', ' '.join(str(x) for x in cmd), flush=True)
    subprocess.run([str(x) for x in cmd], cwd=cwd, env=env, check=True)


def clone_tag(repo: str, tag: str, dst: Path):
    if (dst / '.git').is_dir():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    run(['git', 'clone', '--depth', '1', '--branch', tag, repo, dst])


def cmake_install(src_dir: Path, build_dir: Path, prefix: Path, ndk_root: Path, cmake: str, extra_args: list[str], jobs: int):
    tc = ndk_root / 'build' / 'cmake' / 'android.toolchain.cmake'
    build_dir.mkdir(parents=True, exist_ok=True)
    run([
        cmake, '-S', src_dir, '-B', build_dir,
        f'-DCMAKE_TOOLCHAIN_FILE={tc}',
        '-DANDROID_ABI=arm64-v8a',
        '-DANDROID_PLATFORM=android-29',
        '-DCMAKE_BUILD_TYPE=Release',
        f'-DCMAKE_INSTALL_PREFIX={prefix}',
        '-DCMAKE_C_FLAGS=-march=armv8.2-a+fp16+dotprod',
        '-DCMAKE_CXX_FLAGS=-march=armv8.2-a+fp16+dotprod',
        *extra_args,
    ])
    run([cmake, '--build', build_dir, '--parallel', str(jobs)])
    run([cmake, '--install', build_dir])


def build_libvpx(src_dir: Path, build_dir: Path, prefix: Path, ndk_root: Path, jobs: int):
    ndk_bin = build_game.find_android_ndk_bin(ndk_root)
    assert ndk_bin is not None
    build_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['PATH'] = f'{ndk_bin}{os.pathsep}{env.get("PATH", "")}'
    env['CC'] = str(ndk_bin / 'aarch64-linux-android29-clang')
    env['CXX'] = str(ndk_bin / 'aarch64-linux-android29-clang++')
    env['AR'] = str(ndk_bin / 'llvm-ar')
    env['RANLIB'] = str(ndk_bin / 'llvm-ranlib')
    env['STRIP'] = str(ndk_bin / 'llvm-strip')
    env['AS'] = str(ndk_bin / 'aarch64-linux-android29-clang')
    env['LD'] = str(ndk_bin / 'aarch64-linux-android29-clang++')
    run([
        src_dir / 'configure',
        '--target=arm64-android-gcc',
        f'--prefix={prefix}',
        '--enable-pic',
        '--enable-static',
        '--disable-shared',
        '--enable-vp9-decoder',
        '--disable-vp8',
        '--disable-vp9-encoder',
        '--disable-examples',
        '--disable-tools',
        '--disable-docs',
        '--disable-unit-tests',
        '--disable-webm-io',
    ], cwd=build_dir, env=env)
    run(['make', f'-j{jobs}'], cwd=build_dir, env=env)
    run(['make', 'install'], cwd=build_dir, env=env)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--toolchain', type=Path, help='Android NDK root directory override')
    ap.add_argument('--out', type=Path, default=ROOT / 'toolchain' / 'deps-android',
                    help='install prefix (default toolchain/deps-android)')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    ndk_root = find_ndk_root(args.toolchain)
    cmake = find_cmake()
    prefix = args.out.resolve()
    src_root = ROOT / 'toolchain' / 'src-android'
    bld_root = ROOT / 'toolchain' / 'build-android'
    prefix.mkdir(parents=True, exist_ok=True)

    # 1. SDL3 (shared libSDL3.so + org.libsdl.app Java wrapper classes)
    sdl_src = src_root / 'SDL3'
    clone_tag('https://github.com/libsdl-org/SDL.git', 'release-3.2.8', sdl_src)
    if not (prefix / 'lib' / 'libSDL3.so').is_file():
        cmake_install(sdl_src, bld_root / 'SDL3', prefix, ndk_root, cmake, [
            '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TEST_LIBRARY=OFF', '-DSDL_TESTS=OFF',
        ], args.jobs)
    sdl_java_src = sdl_src / 'android-project' / 'app' / 'src' / 'main' / 'java' / 'org'
    sdl_java_dst = prefix / 'sdl-java' / 'org'
    if sdl_java_src.is_dir():
        if sdl_java_dst.exists():
            shutil.rmtree(sdl_java_dst)
        shutil.copytree(sdl_java_src, sdl_java_dst)
    jni_dir = ROOT / 'android' / 'app' / 'src' / 'main' / 'jniLibs' / 'arm64-v8a'
    jni_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(prefix / 'lib' / 'libSDL3.so', jni_dir / 'libSDL3.so')

    # 2. libopus (static)
    opus_src = src_root / 'opus'
    clone_tag('https://github.com/xiph/opus.git', 'v1.5.2', opus_src)
    if not (prefix / 'lib' / 'libopus.a').is_file():
        cmake_install(opus_src, bld_root / 'opus', prefix, ndk_root, cmake, [
            '-DBUILD_SHARED_LIBS=OFF', '-DOPUS_BUILD_PROGRAMS=OFF', '-DOPUS_BUILD_TESTING=OFF',
        ], args.jobs)
    #Runtime includes both <opus/opus.h> and <opus.h>
    if (prefix / 'include' / 'opus' / 'opus.h').is_file() and not (prefix / 'include' / 'opus.h').is_file():
        for hdr in (prefix / 'include' / 'opus').glob('*.h'):
            shutil.copy2(hdr, prefix / 'include' / hdr.name)

    # 3. zstd (static)
    zstd_src = src_root / 'zstd'
    clone_tag('https://github.com/facebook/zstd.git', 'v1.5.7', zstd_src)
    if not (prefix / 'lib' / 'libzstd.a').is_file():
        cmake_install(zstd_src / 'build' / 'cmake', bld_root / 'zstd', prefix, ndk_root, cmake, [
            '-DZSTD_BUILD_PROGRAMS=OFF', '-DZSTD_BUILD_SHARED=OFF', '-DZSTD_BUILD_STATIC=ON', '-DZSTD_BUILD_TESTS=OFF',
        ], args.jobs)

    # 4. libvpx (static ARM64 NEON VP9 decoder)
    vpx_src = src_root / 'libvpx'
    clone_tag('https://chromium.googlesource.com/webm/libvpx', 'v1.15.0', vpx_src)
    if not (prefix / 'lib' / 'libvpx.a').is_file():
        build_libvpx(vpx_src, bld_root / 'libvpx', prefix, ndk_root, args.jobs)

    # 5. libadrenotools + liblinkernsbypass (shared custom Turnip Vulkan ICD loader for Adreno)
    adreno_src = src_root / 'libadrenotools'
    if not (adreno_src / '.git').is_dir():
        adreno_src.parent.mkdir(parents=True, exist_ok=True)
        run(['git', 'clone', '--recurse-submodules', 'https://github.com/bylaws/libadrenotools.git', adreno_src])
    if not (prefix / 'lib' / 'libadrenotools.so').is_file():
        cmake_install(adreno_src, bld_root / 'libadrenotools', prefix, ndk_root, cmake, [
            '-DBUILD_SHARED_LIBS=ON', '-DGEN_INSTALL_TARGET=ON',
        ], args.jobs)
    for so_name in ('libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so',
                    'libfile_redirect_hook.so', 'libgsl_alloc_hook.so'):
        if (prefix / 'lib' / so_name).is_file():
            shutil.copy2(prefix / 'lib' / so_name, jni_dir / so_name)

    print(f'\nsetup_android_deps: READY at {prefix.relative_to(ROOT)}')
    for item in ('lib/libSDL3.so', 'lib/libadrenotools.so', 'lib/libopus.a', 'lib/libzstd.a', 'lib/libvpx.a',
                 'sdl-java/org/libsdl/app/SDLActivity.java'):
        assert (prefix / item).exists(), f'Missing expected output: {prefix / item}'
        print(f'  [ok] {item}')


if __name__ == '__main__':
    main()
