"""构建 pi_zh.dll 与 pi_zh_loader.exe（MSVC x64）。

用法：
    python src/build.py

依赖：
    Visual Studio 2019/2022 的 MSVC x64 生成工具 + Windows 10/11 SDK。
    找不到编译器时会直接报错退出，不会改动 bin/ 里已有的二进制。

说明：
    早期版本把 VS 路径、SDK 版本和工程根目录写死成作者本机的绝对路径
    （D:\\hc\\wbky\\...），换一台机器必然失败。这里改为按 vswhere / 注册表
    自动探测，工程根目录由本文件位置推导，clone 到任意路径都能构建。
"""

import os
import subprocess
import sys
import glob

try:
    import winreg
except ImportError:      # 非 Windows
    winreg = None

SRC_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SRC_DIR)
BUILD = os.path.join(ROOT, 'build')


def fail(msg):
    print('错误: ' + msg, file=sys.stderr)
    sys.exit(2)


def find_msvc():
    """返回 (MSVC 工具目录, cl.exe 路径)；找不到返回 (None, None)。"""
    # 1) 在 VS 开发者命令提示符里运行时，环境已经就绪
    if os.environ.get('VCToolsInstallDir') and os.environ.get('WindowsSdkDir'):
        vc = os.environ['VCToolsInstallDir'].rstrip('\\')
        cl = os.path.join(vc, 'bin', 'Hostx64', 'x64', 'cl.exe')
        if os.path.exists(cl):
            return vc, cl

    # 2) 用 vswhere 定位最新一个带 C++ 工具的 VS 实例
    for pf in (os.environ.get('ProgramFiles(x86)'), os.environ.get('ProgramFiles')):
        if not pf:
            continue
        vswhere = os.path.join(pf, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
        if not os.path.exists(vswhere):
            continue
        try:
            out = subprocess.run(
                [vswhere, '-latest', '-products', '*',
                 '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                 '-property', 'installationPath'],
                capture_output=True, text=True, timeout=30).stdout.strip()
        except Exception:
            continue
        if not out:
            continue
        for vc in reversed(sorted(glob.glob(os.path.join(out, 'VC', 'Tools', 'MSVC', '*')))):
            cl = os.path.join(vc, 'bin', 'Hostx64', 'x64', 'cl.exe')
            if os.path.exists(cl):
                return vc, cl
    return None, None


def find_sdk():
    """返回 Windows SDK 根目录（含 Include / Lib）；找不到返回 None。"""
    roots = []
    if os.environ.get('WindowsSdkDir'):
        roots.append(os.environ['WindowsSdkDir'])
    if winreg:
        for key in (r'SOFTWARE\Microsoft\Windows Kits\Installed Roots',
                    r'SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots'):
            try:
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as k:
                    roots.append(winreg.QueryValueEx(k, 'KitsRoot10')[0])
            except OSError:
                pass
    for r in roots:
        if r and os.path.isdir(os.path.join(r, 'Include')):
            return r.rstrip('\\')
    return None


def sdk_version(sdk):
    """挑出 Include 下版本号最大的 SDK 版本目录。"""
    vs = [os.path.basename(p) for p in glob.glob(os.path.join(sdk, 'Include', '10.*'))]
    if not vs:
        fail('Windows SDK 目录里没有 10.* 版本: ' + sdk)
    return sorted(vs, key=lambda s: [int(x) if x.isdigit() else 0 for x in s.split('.')])[-1]


def main():
    vc, cl = find_msvc()
    if not cl:
        fail('找不到 MSVC x64 编译器 cl.exe。\n'
             '  请安装 Visual Studio 2022 生成工具（勾选“使用 C++ 的桌面开发”+ Windows SDK），\n'
             '  或在“x64 Native Tools Command Prompt”里执行本脚本。')
    sdk = find_sdk()
    if not sdk:
        fail('找不到 Windows SDK。请安装 Windows 10/11 SDK。')
    ver = sdk_version(sdk)

    env = dict(os.environ)
    env['INCLUDE'] = ';'.join([
        os.path.join(vc, 'include'),
        os.path.join(sdk, 'Include', ver, 'ucrt'),
        os.path.join(sdk, 'Include', ver, 'um'),
        os.path.join(sdk, 'Include', ver, 'shared'),
        os.path.join(sdk, 'Include', ver, 'winrt'),
    ])
    env['LIB'] = ';'.join([
        os.path.join(vc, 'lib', 'x64'),
        os.path.join(sdk, 'Lib', ver, 'ucrt', 'x64'),
        os.path.join(sdk, 'Lib', ver, 'um', 'x64'),
    ])
    env['PATH'] = os.path.join(vc, 'bin', 'Hostx64', 'x64') + ';' + env.get('PATH', '')

    os.makedirs(BUILD, exist_ok=True)
    log_path = os.path.join(BUILD, 'build.log')
    log = open(log_path, 'w', encoding='utf-8')

    def run(cmd):
        print('>>', ' '.join(cmd))
        log.write('>> ' + ' '.join(cmd) + '\n')
        p = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True,
                           text=True, errors='replace')
        print(p.stdout)
        log.write(p.stdout)
        if p.stderr:
            print('ERR:', p.stderr)
            log.write('ERR: ' + p.stderr + '\n')
        log.flush()
        return p.returncode

    print('MSVC   :', vc)
    print('SDK    :', sdk, ver)
    print('ROOT   :', ROOT)

    # 注入 DLL：/MD（目标进程是 Qt6 应用，运行库已在）
    if run([cl, '/utf-8', '/nologo', '/O2', '/GS-', '/EHsc', '/MD', '/LD', '/D_USRDLL',
            os.path.join('src', 'pi_zh.cpp'),
            '/Fo:' + BUILD + '\\', '/Fe:' + os.path.join(BUILD, 'pi_zh.dll'),
            '/link', '/DLL', '/SUBSYSTEM:WINDOWS', '/NOLOGO',
            '/MAP:' + os.path.join(BUILD, 'pi_zh.map')]) != 0:
        fail('pi_zh.dll 编译失败，详见 build/build.log')

    # 启动器：/MT（独立运行，不依赖运行库）
    if run([cl, '/utf-8', '/nologo', '/O2', '/GS-', '/EHsc', '/MT',
            os.path.join('src', 'loader.cpp'),
            '/Fo:' + BUILD + '\\', '/Fe:' + os.path.join(BUILD, 'pi_zh_loader.exe'),
            '/link', '/SUBSYSTEM:WINDOWS', '/NOLOGO']) != 0:
        fail('pi_zh_loader.exe 编译失败，详见 build/build.log')

    # 编译产物同步到 bin/，供安装器取用
    bin_dir = os.path.join(ROOT, 'bin')
    os.makedirs(bin_dir, exist_ok=True)
    for f in ('pi_zh.dll', 'pi_zh_loader.exe'):
        with open(os.path.join(BUILD, f), 'rb') as fi, open(os.path.join(bin_dir, f), 'wb') as fo:
            fo.write(fi.read())
        line = '%s %d 字节 -> bin/' % (f, os.path.getsize(os.path.join(bin_dir, f)))
        print(line)
        log.write(line + '\n')
    log.close()
    print('\n构建完成。记得重新生成 SHA256SUMS.txt。')


if __name__ == '__main__':
    main()
