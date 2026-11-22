// pi_zh_loader.exe - 启动 PixInsight 并注入汉化模块
// 布局: <PI根>\zh_cn\pi_zh_loader.exe + pi_zh.dll + zh_cn.tsv
// 用法: pi_zh_loader.exe [选项] [PixInsight 参数...]
//   --en            以英文原版启动（不注入汉化）
//   --root <目录>   指定 PixInsight 安装目录（含 bin\PixInsight.exe）
//                   也可用环境变量 PI_ZH_ROOT；默认取 zh_cn 的上一级目录
//   --no-doc        不切换 PJSR 帮助文档语言（默认仅在 .zh 存在时才切换）
//   --no-wf         不自动加载 zh_cn\workflow.xpsm（该文件需自备，非随包内容）
//   --help          显示本帮助
// 说明: 本程序不修改 PixInsight 任何文件；删除 zh_cn 目录即恢复原版。
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

static wchar_t g_log[MAX_PATH];
static void lg(const wchar_t* fmt, ...) {
    FILE* f = _wfopen(g_log, L"a, ccs=UTF-8");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfwprintf(f, fmt, ap); va_end(ap);
    fwprintf(f, L"\n"); fclose(f);
}

static void selfdir(wchar_t* out) {
    GetModuleFileNameW(0, out, MAX_PATH);
    wchar_t* s = wcsrchr(out, L'\\'); if (s) *s = 0;
}

// 帮助文档语言切换：PJSR.html 有 .zh/.en 两份，按模式复制到实际路径。
// 只在内容与目标不同时写，避免每次启动都改时间戳。
static void copy_if_diff(const wchar_t* src, const wchar_t* dst) {
    HANDLE s = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (s == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER ss; GetFileSizeEx(s, &ss);
    char* a = (char*)malloc((size_t)ss.QuadPart + 1);
    DWORD rd = 0;
    if (a && ReadFile(s, a, (DWORD)ss.QuadPart, &rd, 0)) {
        a[rd] = 0;
        HANDLE d = CreateFileW(dst, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        bool same = false;
        if (d != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER ds; GetFileSizeEx(d, &ds);
            if (ds.QuadPart == rd) {
                char* b = (char*)malloc((size_t)rd + 1);
                DWORD rd2 = 0;
                if (b && ReadFile(d, b, rd, &rd2, 0)) same = (rd2 == rd && memcmp(a, b, rd) == 0);
                free(b);
            }
            CloseHandle(d);
        }
        if (!same) {
            HANDLE w = CreateFileW(dst, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
            if (w != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(w, a, rd, &wr, 0); CloseHandle(w); }
        }
    }
    free(a);
    CloseHandle(s);
}

static void sync_doc(bool english, const wchar_t* root) {
    wchar_t base[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    swprintf_s(base, L"%ls\\doc\\docs\\PJSR\\PJSR.html", root);
    swprintf_s(src, L"%ls.zh", base);
    if (english) swprintf_s(src, L"%ls.en", base);
    copy_if_diff(src, base);
}

static bool inject(HANDLE proc, const wchar_t* dll) {
    SIZE_T n = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, 0, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    bool ok = false;
    if (WriteProcessMemory(proc, remote, dll, n, 0)) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC fn = GetProcAddress(k32, "LoadLibraryW");
        HANDLE th = CreateRemoteThread(proc, 0, 0, (LPTHREAD_START_ROUTINE)fn, remote, 0, 0);
        if (th) {
            WaitForSingleObject(th, 20000);
            DWORD code = 0; GetExitCodeThread(th, &code);
            CloseHandle(th);
            ok = (code != 0);
        }
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

// ---- 单实例支持 ----
static DWORD g_pids[32];
static int   g_pidn = 0;

static int find_pi(void) {
    g_pidn = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; ZeroMemory(&pe, sizeof(pe)); pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"PixInsight.exe") == 0 && g_pidn < 32)
                g_pids[g_pidn++] = pe.th32ProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return g_pidn;
}

static ULONGLONG ft_now(void) {
    FILETIME f; GetSystemTimeAsFileTime(&f);
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

static ULONGLONG proc_ctime(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME c, e, k, u;
    ULONGLONG r = 0;
    if (GetProcessTimes(h, &c, &e, &k, &u))
        r = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
    CloseHandle(h);
    return r;
}

static BOOL CALLBACK find_main_win(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    for (int i = 0; i < g_pidn; ++i) {
        if (g_pids[i] != pid) continue;
        if (!IsWindowVisible(h) || GetWindowTextLengthW(h) <= 0) continue;
        RECT r; GetWindowRect(h, &r);
        if (r.right - r.left < 600) continue;
        *(HWND*)lp = h;
        return FALSE;
    }
    return TRUE;
}

// 帮助文档是否可切换：只有存在 .zh 副本时才动，纯净安装下完全不写任何外部文件
static bool doc_available(const wchar_t* root) {
    wchar_t p[MAX_PATH];
    swprintf_s(p, L"%ls\\doc\\docs\\PJSR\\PJSR.html.zh", root);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void usage(void) {
    static const wchar_t* txt =
        L"pi_zh_loader.exe - PixInsight 外挂式简体中文启动器\n"
        L"\n"
        L"  pi_zh_loader.exe [选项] [PixInsight 参数...]\n"
        L"\n"
        L"  --en            以英文原版启动（不注入汉化）\n"
        L"  --root <目录>   指定 PixInsight 安装目录（含 bin\\PixInsight.exe）\n"
        L"                  也可用环境变量 PI_ZH_ROOT；默认取 zh_cn 的上一级\n"
        L"  --no-doc        不切换 PJSR 帮助文档语言\n"
        L"  --no-wf         不自动加载 zh_cn\\workflow.xpsm\n"
        L"  --help          显示本帮助\n"
        L"\n"
        L"本程序不修改 PixInsight 任何文件；删除 zh_cn 目录即恢复原版。";
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD wr = 0, mode = 0;
        bool shown = false;
        if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
            shown = WriteConsoleW(h, txt, (DWORD)wcslen(txt), &wr, 0) != 0;
        FreeConsole();
        if (!shown) MessageBoxW(0, txt, L"pi_zh_loader.exe 帮助", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(0, txt, L"pi_zh_loader.exe 帮助", MB_OK | MB_ICONINFORMATION);
    }
}

static int run(int argc, wchar_t** argv) {
    wchar_t dir[MAX_PATH], root[MAX_PATH];
    selfdir(dir);

    BOOL english = FALSE, no_wf = FALSE, no_doc = FALSE;
    BOOL have_root = FALSE;

    // 先扫一遍选项：确定根目录与各开关
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"-h") == 0) { usage(); return 0; }
        if (wcscmp(argv[i], L"--en") == 0)    { english = TRUE; continue; }
        if (wcscmp(argv[i], L"--no-wf") == 0) { no_wf = TRUE;   continue; }
        if (wcscmp(argv[i], L"--no-doc") == 0){ no_doc = TRUE;  continue; }
        if (wcsncmp(argv[i], L"--root=", 7) == 0 && argv[i][7]) {
            wcscpy_s(root, argv[i] + 7); have_root = TRUE; continue;
        }
        if (wcscmp(argv[i], L"--root") == 0 && i + 1 < argc) {
            wcscpy_s(root, argv[++i]); have_root = TRUE; continue;
        }
    }
    if (!have_root) {
        wchar_t env[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"PI_ZH_ROOT", env, MAX_PATH);
        if (n > 0 && n < MAX_PATH) { wcscpy_s(root, env); have_root = TRUE; }
    }
    if (!have_root) {
        wcscpy_s(root, dir);
        wchar_t* s = wcsrchr(root, L'\\'); if (s) *s = 0;
    }
    {   // 去掉尾部反斜杠
        size_t rl = wcslen(root);
        while (rl > 3 && (root[rl - 1] == L'\\' || root[rl - 1] == L'/')) root[--rl] = 0;
    }

    wchar_t exe[MAX_PATH], workdir[MAX_PATH], dll[MAX_PATH], wf[MAX_PATH];
    wcscpy_s(exe, root);     wcscat_s(exe, L"\\bin\\PixInsight.exe");
    wcscpy_s(workdir, root); wcscat_s(workdir, L"\\bin");
    wcscpy_s(dll, dir);      wcscat_s(dll, L"\\pi_zh.dll");
    wcscpy_s(wf, dir);       wcscat_s(wf, L"\\workflow.xpsm");

    wcscpy_s(g_log, dir); wcscat_s(g_log, L"\\loader.log");
    lg(L"exe=%ls workdir=%ls dll=%ls", exe, workdir, dll);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        lg(L"缺少 PixInsight.exe, err=%lu root=%ls", GetLastError(), root);
        wchar_t msg[MAX_PATH + 512];
        swprintf_s(msg,
            L"在以下路径找不到 PixInsight.exe：\n\n%ls\n\n"
            L"请把 zh_cn 目录放到 PixInsight 安装目录下，\n"
            L"或使用  pi_zh_loader.exe --root \"<PixInsight 安装目录>\"\n"
            L"（安装目录应包含 bin\\PixInsight.exe）", exe);
        MessageBoxW(0, msg, L"找不到 PixInsight.exe", MB_ICONERROR);
        return 1;
    }

    wchar_t cmd[8192];
    wcscpy_s(cmd, L"\"");
    wcscat_s(cmd, exe);
    wcscat_s(cmd, L"\"");
    // 再把剩余参数透传给 PixInsight（跳过本程序自己的选项）
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (wcscmp(a, L"--en") == 0 || wcscmp(a, L"--no-wf") == 0 ||
            wcscmp(a, L"--no-doc") == 0 || wcscmp(a, L"--help") == 0 ||
            wcscmp(a, L"-h") == 0) continue;
        if (wcsncmp(a, L"--root=", 7) == 0) continue;
        if (wcscmp(a, L"--root") == 0) { ++i; continue; }
        wcscat_s(cmd, L" ");
        wcscat_s(cmd, a);
    }

    // 帮助文档语言切换：仅在存在 .zh 副本时执行，纯净安装下不写任何外部文件
    if (!no_doc && doc_available(root)) {
        sync_doc(english ? true : false, root);
        lg(L"已切换 PJSR 帮助文档: %ls", english ? L".en" : L".zh");
    }

    // 自动加载工作流：只认 zh_cn\workflow.xpsm，随包不附带，需用户自备
    if (!no_wf && GetFileAttributesW(wf) != INVALID_FILE_ATTRIBUTES) {
        wcscat_s(cmd, L" \"");
        wcscat_s(cmd, wf);
        wcscat_s(cmd, L"\"");
        lg(L"自动加载工作流: %ls", wf);
    }

    // 已有实例：激活窗口；正在启动中的实例耐心等待；只有长期无窗口的死锁进程才清理
    int n = find_pi();
    if (n > 0) {
        HWND w = 0;
        EnumWindows(find_main_win, (LPARAM)&w);
        if (w) {
            lg(L"已在运行 pid=%lu，激活窗口", g_pids[0]);
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
            else             ShowWindow(w, SW_SHOW);
            SetForegroundWindow(w);
            return 0;
        }
        // 无主窗口：区分"正在启动"与"真死锁"
        ULONGLONG now = ft_now(), youngest = 0;
        for (int i = 0; i < n; ++i) {
            ULONGLONG st = proc_ctime(g_pids[i]);
            if (st > youngest) youngest = st;
        }
        double age = (youngest && now > youngest) ? (double)(now - youngest) / 1.0e7 : 1.0e9;
        if (age < 150.0) {
            lg(L"实例正在启动中 pid=%lu 已运行 %.0f 秒，等待其完成，不重复启动", g_pids[0], age);
            return 0;   // 绝不动它
        }
        lg(L"发现 %d 个无窗口死锁进程（已运行 %.0f 秒），清理后重启", n, age);
        for (int i = 0; i < n; ++i) {
            HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, g_pids[i]);
            if (hp) { TerminateProcess(hp, 0); CloseHandle(hp); }
        }
        Sleep(1500);
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    // QtWebEngine 文档视图白屏修复：禁用 GPU 合成（渲染层坏死的机器上必需）
    SetEnvironmentVariableW(L"QTWEBENGINE_CHROMIUM_FLAGS",
        L"--disable-gpu --disable-gpu-compositing --disable-software-rasterizer");
    DWORD flags = english ? 0 : CREATE_SUSPENDED;
    if (!CreateProcessW(exe, cmd, 0, 0, FALSE, flags, 0, workdir, &si, &pi)) {
        lg(L"CreateProcess 失败 err=%lu cmd=%ls", GetLastError(), cmd);
        MessageBoxW(0, L"无法启动 PixInsight.exe", L"汉化启动器", MB_ICONERROR);
        return 1;
    }
    lg(L"CreateProcess 成功 pid=%lu cmd=%ls", pi.dwProcessId, cmd);
    if (!english) {
        bool inj = inject(pi.hProcess, dll);
        lg(L"注入结果=%d err=%lu", (int)inj, GetLastError());
        if (!inj)
            MessageBoxW(0, L"汉化模块注入失败，将以英文原版启动。", L"汉化启动器", MB_ICONWARNING);
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// WINDOWS 子系统入口：双击不弹控制台，启动后与普通程序无异
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = run(argc, argv);
    if (argv) LocalFree(argv);
    return rc;
}
