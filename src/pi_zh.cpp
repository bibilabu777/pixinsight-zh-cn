// pi_zh.dll - PixInsight 运行时界面汉化层
// 不修改任何原始文件：注入进程后对 Qt 文本接口做 IAT 重定向，
// 按 zh_cn.tsv 精确匹配替换显示文本。关闭即恢复英文。
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <string.h>

#pragma comment(lib, "psapi.lib")

typedef unsigned short u16;
typedef void* VP;

// ---------- Qt6Core 字符串接口 ----------
typedef void        (*pfn_fromUtf8)(void* ret, const char* s, long long len);
typedef long long   (*pfn_qsize)(const void* self);
typedef const u16*  (*pfn_utf16)(const void* self);
typedef void        (*pfn_qclear)(void* self);
static pfn_fromUtf8 q_fromUtf8 = 0;
static pfn_qsize    q_size = 0;
static pfn_utf16    q_utf16 = 0;

// ---------- 词典 ----------
struct KeyEnt { unsigned int h; int idx; };
static std::vector<KeyEnt>       g_tab;
static unsigned int              g_mask = 0;
static std::vector<std::wstring> g_keys;
static std::vector<void*>        g_vals;
static std::vector<std::string>  g_utf8;
static volatile LONG             g_ready = 0;

static unsigned int hashw(const u16* p, int n) {
    unsigned int h = 2166136261u;
    for (int i = 0; i < n; ++i) { h ^= (unsigned int)p[i]; h *= 16777619u; }
    h ^= (unsigned int)n; h *= 16777619u;
    return h ? h : 1u;
}

static void selfdir(wchar_t* out) {
    HMODULE hm = 0;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&selfdir, &hm);
    GetModuleFileNameW(hm, out, MAX_PATH);
    wchar_t* s = wcsrchr(out, L'\\'); if (s) *s = 0;
}

static void logline(const char* fmt, ...) {
    char buf[1200];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = sprintf_s(buf, sizeof(buf), "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\r'; buf[n++] = '\n';
    wchar_t dir[MAX_PATH]; selfdir(dir);
    wcscat_s(dir, MAX_PATH, L"\\pi_zh.log");
    HANDLE h = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0; WriteFile(h, buf, (DWORD)n, &w, 0); CloseHandle(h);
}

// ---------- 富文本工具提示：去标签 + 实体解码 + 空白归一化 ----------
// PI 的悬浮提示是 HTML 富文本（<i>/<b>/<p>、&nbsp; 等），词典存的是纯文本，
// 必须归一化后才能和词典 key 对上。
static void norm_tip(const std::wstring& src, std::wstring& out) {
    out.clear();
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        wchar_t c = src[i];
        if (c == L'<') { while (i < src.size() && src[i] != L'>') ++i; continue; }
        if (c == L'&') {
            if (src.compare(i, 6, L"&nbsp;") == 0)       { c = L' '; i += 5; }
            else if (src.compare(i, 5, L"&amp;") == 0)   { c = L'&'; i += 4; }
            else if (src.compare(i, 4, L"&lt;") == 0)    { c = L'<'; i += 3; }
            else if (src.compare(i, 4, L"&gt;") == 0)    { c = L'>'; i += 3; }
            else if (src.compare(i, 6, L"&quot;") == 0)  { c = L'"'; i += 5; }
            else if (src.compare(i, 6, L"&#160;") == 0)  { c = L' '; i += 5; }
            else if (src.compare(i, 5, L"&#39;") == 0)   { c = L'\''; i += 4; }
            else if (src.compare(i, 7, L"&mdash;") == 0) { c = (wchar_t)0x2014; i += 6; }
            else if (src.compare(i, 7, L"&ndash;") == 0) { c = (wchar_t)0x2013; i += 6; }
            else if (src.compare(i, 7, L"&rsquo;") == 0) { c = (wchar_t)0x2019; i += 6; }
            else if (src.compare(i, 7, L"&lsquo;") == 0) { c = (wchar_t)0x2018; i += 6; }
            else if (src.compare(i, 7, L"&rdquo;") == 0) { c = (wchar_t)0x201D; i += 6; }
            else if (src.compare(i, 7, L"&ldquo;") == 0) { c = (wchar_t)0x201C; i += 6; }
            else if (src.compare(i, 8, L"&hellip;") == 0) { c = (wchar_t)0x2026; i += 7; }
        }
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == (wchar_t)0xA0) c = L' ';
        if (c == L' ') { if (out.empty() || out.back() == L' ') continue; }
        out += c;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
}

// 词典里记的是 "&lt;0.13"（无空格），而原文 "&lt; 0.13" 解码后是 "< 0.13"（有空格），
// 只差一个空格就会整句匹配失败。整句查不到时，去掉 < > 两侧空白再查一次。
static void compact_ops(std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        // 只删 < > 之后的空白，保留之前的（词典键里 "<0.13" 前面那个空格是有的）
        if ((c == L' ' || c == L'\t') && !o.empty() && (o.back() == L'<' || o.back() == L'>')) continue;
        o += c;
    }
    s.swap(o);
}

// 未命中词典的富文本提示原文落盘（只写本 DLL 目录下的日志，不碰 PI 任何文件）
static std::mutex g_tipmu;
static std::unordered_set<std::wstring> g_tipseen;
static int g_tipcount = 0;

static void tiplog(const std::wstring& raw, const std::wstring& plain) {
    std::lock_guard<std::mutex> g(g_tipmu);
    if (g_tipcount >= 400) return;
    if (!g_tipseen.insert(raw).second) return;
    ++g_tipcount;
    std::wstring line = std::wstring(L"=== RAW ===\r\n") + raw +
                        L"\r\n=== PLAIN ===\r\n" + plain + L"\r\n";
    int cn = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), 0, 0, 0, 0);
    if (cn <= 0) return;
    std::string u8((size_t)cn, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &u8[0], cn, 0, 0);
    wchar_t dir[MAX_PATH]; selfdir(dir);
    wcscat_s(dir, MAX_PATH, L"\\tip_raw.txt");
    HANDLE h = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, u8.data(), (DWORD)u8.size(), &wr, 0); CloseHandle(h);
}

// 纯文本未命中诊断（仅 PI_ZH_DIAG=1 时启用）：把长英文纯文本提示落盘，
// 用于确认真实的 tooltip 原文。只写本 DLL 目录，不碰 PI 任何文件。
static volatile LONG g_diag = 0;
static thread_local const char* g_hn = "?";   // 当前命中的钩子名（诊断用）
static std::mutex g_plmu;
static std::unordered_set<std::wstring> g_plseen;
static int g_plcount = 0;

static void plainlog(const std::wstring& w) {
    if (!g_diag) return;
    if (w.size() < 20 || w.size() > 1900) return;
    for (size_t i = 0; i < w.size(); ++i) {
        wchar_t c = w[i];
        if (c < 32 || c >= 127) return;          // 只记纯 ASCII 英文串
    }
    std::lock_guard<std::mutex> g(g_plmu);
    if (g_plcount >= 300) return;
    if (!g_plseen.insert(w).second) return;
    ++g_plcount;
    int cn = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), 0, 0, 0, 0);
    if (cn <= 0) return;
    std::string u8((size_t)cn, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &u8[0], cn, 0, 0);
    u8 += "\r\n";
    wchar_t dir[MAX_PATH]; selfdir(dir);
    wcscat_s(dir, MAX_PATH, L"\\tip_plain.txt");
    HANDLE h = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, u8.data(), (DWORD)u8.size(), &wr, 0); CloseHandle(h);
}

// 诊断：记录所有长度>=100 的字符串是否进入了钩子（用于判定 tooltip 是否可达）
static std::mutex g_anmu;
static std::unordered_set<std::wstring> g_anseen;
static int g_ancount = 0;

static void anylog(const std::wstring& w, const char* tag) {
    if (!g_diag) return;
    std::lock_guard<std::mutex> g(g_anmu);
    if (g_ancount >= 400) return;
    if (!g_anseen.insert(w).second) return;
    ++g_ancount;
    std::wstring line = std::wstring(L"### ") + std::wstring(tag, tag + strlen(tag)) + L" ###\r\n" + w + L"\r\n";
    int cn = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), 0, 0, 0, 0);
    if (cn <= 0) return;
    std::string u8((size_t)cn, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &u8[0], cn, 0, 0);
    wchar_t dir[MAX_PATH]; selfdir(dir);
    wcscat_s(dir, MAX_PATH, L"\\tip_any.txt");
    HANDLE h = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, u8.data(), (DWORD)u8.size(), &wr, 0); CloseHandle(h);
}

// ---------- 定向探针（PI_ZH_PROBE=1）----------
// 只盯着「已知未翻译」的那几条 tooltip，记录它们究竟有没有进钩子、进的是哪个钩子。
// 用于判定：是没被拦截，还是拦截了但替换结果被丢弃。
static volatile LONG g_probe = 0;
static std::mutex g_prmu;
static std::unordered_set<std::wstring> g_prseen;
static int g_prcount = 0;

static const wchar_t* g_pmark[] = {
    L"You can choose among",        L"Search radius in degrees",
    L"compound angular quantities", L"right ascension coordinate will be",
    L"Lower bound of the search",   L"Upper bound of the search",
    L"maximum number of sources",   L"output plain text file",
    L"format of generated output",  L"Required Flags",
    L"No proper motions",           L"Bronze GRP",
};
static const int NPMARK = (int)(sizeof(g_pmark) / sizeof(g_pmark[0]));

static void prwrite(const std::wstring& line) {
    int cn = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), 0, 0, 0, 0);
    if (cn <= 0) return;
    std::string u8((size_t)cn, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &u8[0], cn, 0, 0);
    wchar_t dir[MAX_PATH]; selfdir(dir);
    wcscat_s(dir, MAX_PATH, L"\\probe.txt");
    HANDLE h = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, u8.data(), (DWORD)u8.size(), &wr, 0); CloseHandle(h);
}

static void probecheck(const u16* p, long long n) {
    if (!g_probe) return;
    std::wstring w((const wchar_t*)p, (size_t)n);
    // 心跳：某钩子首次被调用就记一行，用于区分「钩子没跑」和「文本没经过钩子」
    {
        std::lock_guard<std::mutex> g(g_prmu);
        static std::unordered_set<std::string> hb;
        if (hb.insert(std::string(g_hn)).second) {
            prwrite(std::wstring(L"## heartbeat hook=") + std::wstring(g_hn, g_hn + strlen(g_hn)) +
                    L" n=" + std::to_wstring(n) + L"\r\n");
        }
    }
    if (n < 40) return;
    // 只要含 ASCII 字母、不含中日韩，就算一条候选（用于定位 tooltip 走哪条路径）
    bool alpha = false;
    for (long long i = 0; i < n; ++i) {
        wchar_t c = (wchar_t)p[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) alpha = true;
        if (c >= 0x2E80) return;
    }
    if (!alpha) return;
    std::lock_guard<std::mutex> g(g_prmu);
    if (g_prcount >= 800) return;
    std::wstring key = std::wstring(g_hn, g_hn + strlen(g_hn)) + L"|" + w;
    if (!g_prseen.insert(key).second) return;
    ++g_prcount;
    std::wstring body = w.size() > 500 ? w.substr(0, 500) : w;
    std::wstring line = std::wstring(L"### hook=") + std::wstring(g_hn, g_hn + strlen(g_hn)) +
                        L" len=" + std::to_wstring(n) + L" ###\r\n" + body + L"\r\n";
    prwrite(line);
}

static char* slurp(const wchar_t* path, DWORD* outlen) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(f, 0), got = 0;
    if (sz == INVALID_FILE_SIZE || sz > (64u << 20)) { CloseHandle(f); return 0; }
    char* b = (char*)malloc(sz + 4);
    if (!ReadFile(f, b, sz, &got, 0)) { CloseHandle(f); free(b); return 0; }
    CloseHandle(f);
    b[got] = 0; b[got + 1] = 0;
    if (got >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) {
        memmove(b, b + 3, got - 2); got -= 3;
    }
    *outlen = got; return b;
}

static int findkey(const wchar_t* k, long long n);   // 前向声明：词典加载后自检用

static void builddict() {
    wchar_t dir[MAX_PATH]; selfdir(dir);
    wchar_t path[MAX_PATH]; wcscpy_s(path, dir); wcscat_s(path, L"\\zh_cn.tsv");
    DWORD len = 0; char* b = slurp(path, &len);
    if (!b) { logline("dictionary missing"); return; }

    std::vector<std::pair<std::wstring, std::string> > pairs;
    pairs.reserve(8192);
    int skipped_long = 0;      // 超过查表上限被丢弃的键（应为 0，非 0 说明上限又要调）
    int max_key = 0;           // 最长键长度，便于判断上限是否需要再放开
    int longest_idx = -1;      // 最长键在 pairs 里的位置，加载后做一次自检
    char* p = b, *end = b + len;
    while (p < end) {
        char* nl = p; while (nl < end && *nl != '\n') ++nl;
        char save = *nl; *nl = 0;
        char* line = p;
        while (*line == ' ' || *line == '\t') ++line;
        if (*line && *line != '#') {
            char* tab = 0;
            for (char* q = line; *q; ++q) if (*q == '\t') { tab = q; break; }
            if (tab) {
                *tab = 0; char* zh = tab + 1;
                int el = (int)strlen(line);
                while (el > 0 && (line[el-1] == ' ' || line[el-1] == '\r')) line[--el] = 0;
                int zl = (int)strlen(zh);
                while (zl > 0 && (zh[zl-1] == '\r' || zh[zl-1] == ' ')) zh[--zl] = 0;
                if (el > 0 && zl > 0) {
                    int wn = MultiByteToWideChar(CP_UTF8, 0, line, el, 0, 0);
                    // 上限与 T() 的查表上限一致（8000 wchar）。早期版本卡在 1024，
                    // 之后又卡在 2000，导致超长富文本 tooltip（StarAlignment / WBPP /
                    // 马赛克 / 图像解算 / 渲染意图）被静默丢弃或放行，界面始终显示英文。
                    if (wn > max_key) { max_key = wn; longest_idx = (int)pairs.size(); }
                    if (wn > 0 && wn < 8000) {
                        std::wstring w(wn, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, line, el, &w[0], wn);
                        pairs.push_back(std::make_pair(w, std::string(zh, zl)));
                    } else if (wn >= 8000) {
                        ++skipped_long;
                    }
                }
            }
        }
        *nl = save;
        p = nl + 1;
    }
    free(b);

    unsigned int cap = 64;
    while (cap < pairs.size() * 2 + 32) cap <<= 1;
    g_tab.assign(cap, KeyEnt());
    g_mask = cap - 1;
    g_keys.reserve(pairs.size());
    g_vals.reserve(pairs.size());
    g_utf8.reserve(pairs.size());

    int added = 0, failed = 0;
    for (size_t i = 0; i < pairs.size(); ++i) {
        void* qs = calloc(1, 64);
        q_fromUtf8(qs, pairs[i].second.c_str(), (long long)pairs[i].second.size());
        if (q_size(qs) <= 0) { free(qs); ++failed; continue; }
        int idx = (int)g_keys.size();
        g_keys.push_back(pairs[i].first);
        g_vals.push_back(qs);
        g_utf8.push_back(pairs[i].second);
        unsigned int h = hashw((const u16*)pairs[i].first.c_str(), (int)pairs[i].first.size());
        unsigned int s = h & g_mask;
        while (g_tab[s].h) s = (s + 1) & g_mask;
        g_tab[s].h = h; g_tab[s].idx = idx;
        ++added;
    }
    logline("dict loaded: %d entries, %d failed, maxkey=%d, skipped_long=%d", added, failed, max_key, skipped_long);
    // 自检：最长键能否被 findkey 查到。查不到说明哈希表/装载有 bug，长提示永远翻不出来。
    if (longest_idx >= 0 && longest_idx < (int)g_keys.size()) {
        const std::wstring& lk = g_keys[(size_t)longest_idx];
        int hit = findkey(lk.c_str(), (long long)lk.size());
        logline("selftest longest key: len=%d hit=%d", (int)lk.size(), hit >= 0 ? 1 : 0);
    }
    // 自检 2：尾部冒号回退。RC-Astro 三件套的参数标签是「显示名 + ": "」形式，
    // 词典里只存显示名。这里按 T() 的同一套剥离规则验证 findkey 能否命中，
    // hit=1 说明回退链路的核心步骤有效（界面上这些标签会被翻成「译名: 」）。
    {
        const wchar_t* probes[] = {
            L"Denoise Intensity: ",
            L"Sharpen Stars: ",
            L"Tile Overlap: ",
            L"HF/LF Scale (pixels): ",
        };
        for (int i = 0; i < 4; ++i) {
            std::wstring s = probes[i];
            long long m = (long long)s.size();
            while (m > 0 && (s[(size_t)m - 1] == L' ' || s[(size_t)m - 1] == L'\t')) --m;
            long long c = m;
            while (c > 0 && (s[(size_t)c - 1] == L':' || s[(size_t)c - 1] == L'\uff1a')) --c;
            long long k = c;
            while (k > 0 && (s[(size_t)k - 1] == L' ' || s[(size_t)k - 1] == L'\t')) --k;
            int hit = (k >= 2) ? findkey(s.c_str(), k) : -1;
            logline("selftest colon-fallback: len=%d hit=%d", (int)k, hit >= 0 ? 1 : 0);
        }
    }
    // 自检 3：助记符回退。Qt 标准菜单/按钮传 "&Cut" / "&OK" 这类带 & 的文本，
    // 词典里只有无 & 的显示名。剥 & 后必须命中。
    {
        const wchar_t* probes[] = {
            L"&Cut", L"&Copy", L"&Paste", L"&Select All",
            L"&OK", L"&Cancel", L"&Yes", L"&No",
            L"&Save", L"&Open", L"&Close", L"&Float",
            L"&New Folder", L"&Files of type: ",
        };
        for (int i = 0; i < 14; ++i) {
            std::wstring s = probes[i], bare;
            for (size_t j = 0; j < s.size(); ++j) {
                if (s[j] != L'&') bare.push_back(s[j]);
            }
            long long m2 = (long long)bare.size();
            while (m2 > 0 && (bare[(size_t)(m2 - 1)] == L' ' || bare[(size_t)(m2 - 1)] == L'\t')) --m2;
            if (m2 > 0 && bare[(size_t)(m2 - 1)] == L':') {
                while (m2 > 0 && bare[(size_t)(m2 - 1)] == L':') --m2;
                while (m2 > 0 && (bare[(size_t)(m2 - 1)] == L' ' || bare[(size_t)(m2 - 1)] == L'\t')) --m2;
            }
            int hit = (m2 >= 2) ? findkey(bare.c_str(), m2) : -1;
            logline("selftest amp-fallback: [%ls] hit=%d", probes[i], hit >= 0 ? 1 : 0);
        }
    }
}

// ---------- 采集模式 ----------
static std::unordered_set<std::wstring>* g_seen = 0;
static std::unordered_set<std::wstring>* g_miss = 0;   // 未命中词典的界面英文
static CRITICAL_SECTION g_cs;
static int g_seencount = 0;
static int g_misscount = 0;
static volatile LONG g_tcalls = 0;

// 未命中词典的真实界面文本落盘，用于补齐词条
static void missrec(const u16* p, int n) {
    // 上限放宽到 2000：脚本类长 tooltip 常有 600~1000 字符，
    // 旧的 300 会把它们整条挡掉，导致「日志里看不到但界面上确实是英文」。
    if (!g_miss || n < 2 || n > 2000) return;
    // 只记录以拉丁字母为主、且不像纯中文的串
    bool alpha = false;
    int nonlatin = 0;
    for (int i = 0; i < n; ++i) {
        wchar_t c = (wchar_t)p[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) alpha = true;
        if (c >= 0x0400) ++nonlatin;
    }
    if (!alpha) return;
    // 允许少量中文（例如 tooltip 里拼接的参数名），但主体必须是英文
    if (nonlatin * 3 > n) return;
    EnterCriticalSection(&g_cs);
    if (g_misscount < 16000) {
        std::wstring w((const wchar_t*)p, n);
        if (g_miss->insert(w).second) {
            ++g_misscount;
            wchar_t dir[MAX_PATH]; selfdir(dir);
            wcscat_s(dir, MAX_PATH, L"\\pi_miss.txt");
            char u8[8192];
            int cn = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), u8, (int)sizeof(u8) - 2, 0, 0);
            if (cn > 0) {
                u8[cn++] = '\r'; u8[cn++] = '\n';
                HANDLE h = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
                if (h != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(h, u8, (DWORD)cn, &wr, 0); CloseHandle(h); }
            }
        }
    }
    LeaveCriticalSection(&g_cs);
}

static void record(const u16* p, int n) {
    if (!g_seen || n < 2 || n > 2000) return;
    EnterCriticalSection(&g_cs);
    if (g_seencount < 16000) {
        std::wstring w((const wchar_t*)p, n);
        if (g_seen->insert(w).second) {
            ++g_seencount;
            wchar_t dir[MAX_PATH]; selfdir(dir);
            wcscat_s(dir, MAX_PATH, L"\\pi_seen.txt");
            char u8[8192];
            int cn = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), u8, (int)sizeof(u8) - 2, 0, 0);
            if (cn > 0) {
                u8[cn++] = '\r'; u8[cn++] = '\n';
                HANDLE h = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
                if (h != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(h, u8, (DWORD)cn, &wr, 0); CloseHandle(h); }
            }
        }
    }
    LeaveCriticalSection(&g_cs);
}


// 译文对象缓存：内容寻址、写入后永不覆盖，保证同一译文始终返回同一对象指针
static void*  g_cptr[16384];
static unsigned int g_chash[16384];
static std::string g_ctext[16384];

static CRITICAL_SECTION g_qcs;
static bool g_qcs_ok = false;

static void* qcache_str(const std::string& t) {
    unsigned int h = 2166136261u;
    for (size_t i = 0; i < t.size(); ++i) { h ^= (unsigned char)t[i]; h *= 16777619u; }
    h ^= (unsigned int)t.size(); h *= 16777619u;
    if (g_qcs_ok) EnterCriticalSection(&g_qcs);
    unsigned int slot = h & 16383u;
    for (int guard = 0; guard < 64; ++guard) {
        unsigned int s = (slot + guard) & 16383u;
        if (!g_cptr[s]) {
            void* qs = calloc(1, 64);
            q_fromUtf8(qs, t.c_str(), (long long)t.size());
            if (q_size(qs) <= 0) { free(qs); if (g_qcs_ok) LeaveCriticalSection(&g_qcs); return 0; }
            g_ctext[s] = t;
            g_chash[s] = h;
            g_cptr[s] = qs;
            if (g_qcs_ok) LeaveCriticalSection(&g_qcs);
            return qs;
        }
        if (g_chash[s] == h && g_ctext[s] == t) {
            void* r = g_cptr[s];
            if (g_qcs_ok) LeaveCriticalSection(&g_qcs);
            return r;
        }
    }
    if (g_qcs_ok) LeaveCriticalSection(&g_qcs);
    return 0;
}

static void* qcache(int idx) { return qcache_str(g_utf8[idx]); }

// ---------- 长提示折行 ----------
// tooltip 原文是 HTML 多段（<p>...</p>），而词典键是剥标签后的纯文本；
// 译文返回纯文本后所有段落被压成一整块，屏幕上就是一大片密集文字。
// 处理方式（只对富文本长提示生效，不碰按钮/标签类短文本）：
//   按显示宽度打包，每行上限 TIP_MAXCOL 列（中文按 2 列）；
//   换行点优先落在句末标点，其次分句标点，再次空格，都没有才硬断；
//   断点必须用掉至少 40% 行宽，否则宁可硬断，避免右侧大片留白；
//   英文单词内部不硬断（允许小幅超宽）。
#define TIP_MAXCOL 72
#define TIP_OVERFLOW 96

static int utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    return 4;
}

static bool is_alpha_ch(const std::string& c) {
    if (c.size() != 1) return false;
    char x = c[0];
    return (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z');
}

static bool is_punct_lead(const std::string& s, size_t pos) {
    if (pos + 3 > s.size()) return false;
    std::string t = s.substr(pos, 3);
    return t == "\xE3\x80\x82" || t == "\xE3\x80\x81" ||   // 。 、
           t == "\xEF\xBC\x8C" || t == "\xEF\xBC\x9B" ||   // ， ；
           t == "\xEF\xBC\x9A" || t == "\xEF\xBC\x81" ||   // ： ！
           t == "\xEF\xBC\x9F";                            // ？
}

// 行首标点接到上一行末尾，避免标点悬挂在行首
static std::string fix_hang(const std::string& in) {
    std::string o;
    o.reserve(in.size());
    size_t p = 0;
    while (p < in.size()) {
        if (in[p] == '\n' && is_punct_lead(in, p + 1)) {
            o += in.substr(p + 1, 3);
            o += '\n';
            p += 4;
            continue;
        }
        o += in[p];
        ++p;
    }
    return o;
}

// 判定字符串里是否含真正的 HTML 标签。
// 不能只看有没有 '<'：译文里的 <target image>、<system-temp-dir>、<无图像> 是占位符，
// 既有尖括号也不是标签。误判成标签会让 Qt 按富文本渲染，换行全部失效、提示被撑爆。
static bool has_real_tag(const std::string& s) {
    static const char* names[] = {
        "p","b","i","u","br","a","em","strong","span","font","div","center",
        "ul","ol","li","table","tr","td","th","h1","h2","h3","h4","h5","h6",
        "img","sub","sup","pre","code","blockquote","hr","dl","dt","dd",
        "tt","big","small","nobr","html","body","head","title","meta","style"
    };
    const size_t NN = sizeof(names) / sizeof(names[0]);
    size_t i = 0;
    while ((i = s.find('<', i)) != std::string::npos) {
        size_t j = i + 1;
        if (j < s.size() && s[j] == '/') ++j;
        size_t ns = j;
        while (j < s.size()) {
            char c = s[j];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ++j;
            else break;
        }
        if (j > ns) {
            std::string nm = s.substr(ns, j - ns);
            for (size_t k = 0; k < NN; ++k) {
                size_t ln = strlen(names[k]);
                if (nm.size() == ln && _stricmp(nm.c_str(), names[k]) == 0) return true;
            }
        }
        i = (j > i) ? j : i + 1;
    }
    return false;
}

// 把裸尖括号转成 HTML 实体。只处理 < 和 >，不动 & ——译文里已有 &mdash; 等实体，
// 一并转义会变成 &amp;mdash; 而显示成字面量。
static std::string escape_angles(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 32);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') o += "&lt;";
        else if (s[i] == '>') o += "&gt;";
        else o += s[i];
    }
    return o;
}

// 含占位符的译文：按中文句末标点拆成多个 <p> 段落，行内换行交给 Qt。
// 早先版本是自己算列宽、插 <br/> 再返回富文本。问题在于 QTipLabel 的尺寸计算
// 与 <br/> 实际渲染不一致：宽度按最长一行算、高度却只按极少数行算，结果提示框
// 只画出一两行，后面的内容被整块裁掉（"背景中和 → 参考图像" 的悬浮就是这样，
// 屏幕上只剩最后一句）。改成多段 <p> 后由 QTextDocument 自己排版，宽高必然自洽。
static std::string split_paras(const std::string& s) {
    std::string e = escape_angles(s);
    std::string o = "<p>";
    o.reserve(e.size() + 32);
    size_t i = 0;
    while (i < e.size()) {
        int L = utf8_len((unsigned char)e[i]);
        if (L <= 0 || i + (size_t)L > e.size()) L = 1;
        o.append(e, i, (size_t)L);
        i += (size_t)L;
        if (L == 3 && i < e.size()) {
            bool stop = (e.compare(i - 3, 3, "\xE3\x80\x82") == 0) ||   // 。
                        (e.compare(i - 3, 3, "\xEF\xBC\x81") == 0) ||   // ！
                        (e.compare(i - 3, 3, "\xEF\xBC\x9F") == 0);     // ？
            if (stop) o += "</p><p>";
        }
    }
    o += "</p>";
    return o;
}

// 占位符（<target image>、<system-temp-dir>）不要在中间被折断：
// 若某行行尾出现未闭合的 '<'，把这一段挪到下一行开头。
static std::string fix_placeholder(const std::string& in) {
    std::string o;
    o.reserve(in.size() + 16);
    size_t pos = 0;
    while (pos < in.size()) {
        size_t nl = in.find('\n', pos);
        std::string line = (nl == std::string::npos) ? in.substr(pos)
                                                    : in.substr(pos, nl - pos);
        size_t lt = line.rfind('<');
        size_t gt = line.rfind('>');
        bool open = (lt != std::string::npos) && (gt == std::string::npos || gt < lt);
        if (open && lt > 0 && line.size() - lt <= 40) {
            o.append(line, 0, lt);                     // 断在 '<' 之前
            o += '\n';
            o.append(line, lt, std::string::npos);     // 占位符留到下一行开头
        } else {
            o += line;
            if (nl != std::string::npos) o += '\n';
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return o;
}

static std::string wrap_tip(const std::string& s) {
    if (s.size() < 60) return s;                        // 短文本不处理
    if (s.find('\n') != std::string::npos) return s;    // 已含换行
    if (has_real_tag(s)) return s;                      // 真标签由 Qt 富文本分段，别再插换行
    bool hasCjkStop = (s.find("\xE3\x80\x82") != std::string::npos) ||   // 。
                      (s.find("\xEF\xBC\x9B") != std::string::npos);     // ；
    if (!hasCjkStop) return s;

    std::vector<std::string> cur;      // 当前行的字符
    std::vector<int>         curw;     // 对应显示宽度
    std::string out;
    out.reserve(s.size() + s.size() / 24);
    int col = 0, cutIdx = -1, cutLv = 0;
    size_t i = 0;

    while (i < s.size()) {
        int L = utf8_len((unsigned char)s[i]);
        if (i + (size_t)L > s.size()) L = 1;
        std::string ch = s.substr(i, (size_t)L);
        i += (size_t)L;
        int w = (L >= 2) ? 2 : 1;
        cur.push_back(ch); curw.push_back(w); col += w;

        int lv = 0;
        if (ch == "\xE3\x80\x82" || ch == "\xEF\xBC\x81" ||
            ch == "\xEF\xBC\x9F" || ch == "\xEF\xBC\x9B") lv = 3;   // 句末
        else if (ch == "\xEF\xBC\x8C" || ch == "\xE3\x80\x81" ||
                 ch == "\xEF\xBC\x9A") lv = 2;                      // 分句
        else if (ch == " ") lv = 1;                                 // 空格
        if (lv > 0 && lv >= cutLv) { cutIdx = (int)cur.size(); cutLv = lv; }

        if (col < TIP_MAXCOL) continue;

        // 英文单词中间不断开：连续字母允许超宽到 TIP_OVERFLOW
        if (col < TIP_OVERFLOW && is_alpha_ch(ch) && cur.size() >= 2 &&
            is_alpha_ch(cur[cur.size() - 2])) continue;

        int at = (int)cur.size();
        if (cutIdx > 0) {
            int hw = 0;
            for (int k = 0; k < cutIdx; ++k) hw += curw[(size_t)k];
            if (hw * 10 >= col * 4) at = cutIdx;     // 断点至少用掉 40% 行宽
        }
        for (int k = 0; k < at; ++k) out += cur[(size_t)k];
        out += '\n';
        cur.erase(cur.begin(), cur.begin() + at);
        curw.erase(curw.begin(), curw.begin() + at);
        col = 0;
        for (size_t k = 0; k < curw.size(); ++k) col += curw[k];
        cutIdx = -1; cutLv = 0;
    }
    for (size_t k = 0; k < cur.size(); ++k) out += cur[k];
    while (!out.empty() && out[out.size() - 1] == '\n') out.resize(out.size() - 1);

    bool bare = (s.find('<') != std::string::npos);   // 含占位符尖括号（非真标签）
    if (bare) return split_paras(s);                  // 交给 Qt 按段排版，不自己插换行
    if (out.find('\n') == std::string::npos) return s;
    out = fix_hang(out);
    return out;
}

// 富文本 tooltip 专用：命中译文后按句读折行再缓存
static void* qcache_tip(int idx) { return qcache_str(wrap_tip(g_utf8[idx])); }

// ---------- 拼接型窗口标题 ----------
// ChannelCombination 等进程会按当前色彩空间动态拼出对话框标题，例如
//   Select RGB Channel #0 Source Image / Select CIE XYZ Channel #1 Source Image
// 色彩空间名可变、编号可变，无法用精确词典覆盖，这里按模式转换。
static std::string w2u8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int cn = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), 0, 0, 0, 0);
    if (cn <= 0) return std::string();
    std::string s((size_t)cn, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], cn, 0, 0);
    return s;
}

static bool title_pat(const std::wstring& w, std::wstring& out) {
    static const std::wstring PRE = L"Select ";
    static const std::wstring MID = L" Channel #";
    static const std::wstring SUF = L" Source Image";
    if (w.size() < PRE.size() + MID.size() + SUF.size()) return false;
    if (w.compare(0, PRE.size(), PRE) != 0) return false;
    if (w.compare(w.size() - SUF.size(), SUF.size(), SUF) != 0) return false;
    size_t m = w.find(MID);
    if (m == std::wstring::npos) return false;
    // "Select" 后可以直接跟 " Channel #..."（没有色彩空间名），此时 m == 6
    if (m + 1 < PRE.size()) return false;
    size_t ns = m + MID.size();
    size_t ne = w.size() - SUF.size();
    if (ne <= ns || ne - ns > 3) return false;
    for (size_t i = ns; i < ne; ++i)
        if (w[i] < L'0' || w[i] > L'9') return false;
    std::wstring cs;
    if (m > PRE.size()) cs = w.substr(PRE.size(), m - PRE.size());
    std::wstring num = w.substr(ns, ne - ns);
    out = cs.empty() ? (L"选择通道 #" + num + L" 源图像")
                     : (L"选择 " + cs + L" 通道 #" + num + L" 源图像");
    return true;
}

// ---------- 白平衡三通道提示 ----------
// AssistedColorCalibration 的白平衡系数控件，其 tooltip 是运行时拼出来的：
//   "<p>" + {Red|Green|Blue} + " channel correction factor.</p>"
// 模块里只存了中间片段 " channel correction factor.</p>"，词典没有完整键，
// 所以三个控件的悬浮一直是英文。这里按特征片段识别并用对应颜色名生成译文。
static size_t wfind_ci(const std::wstring& w, const wchar_t* k) {
    size_t kl = wcslen(k);
    if (kl == 0 || w.size() < kl) return std::wstring::npos;
    for (size_t i = 0; i + kl <= w.size(); ++i) {
        size_t j = 0;
        for (; j < kl; ++j) {
            wchar_t a = w[i + j], b = k[j];
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
            if (b >= L'A' && b <= L'Z') b = (wchar_t)(b - L'A' + L'a');
            if (a != b) break;
        }
        if (j == kl) return i;
    }
    return std::wstring::npos;
}

static bool wb_tip_pat(const std::wstring& w, std::wstring& out) {
    if (w.size() > 120) return false;
    if (wfind_ci(w, L"channel correction factor") == std::wstring::npos) return false;
    static const wchar_t* EN[3] = { L"red", L"green", L"blue" };
    static const wchar_t* ZH[3] = { L"\u7ea2\u8272", L"\u7eff\u8272", L"\u84dd\u8272" };
    for (int i = 0; i < 3; ++i) {
        if (wfind_ci(w, EN[i]) == std::wstring::npos) continue;
        out = std::wstring(L"<p>") + ZH[i] + L"\u901a\u9053\u6821\u6b63\u7cfb\u6570\u3002</p>";
        return true;
    }
    return false;
}

// ---------- 词典查找（供动态串复用） ----------
static int findkey(const wchar_t* k, long long n) {
    if (g_keys.empty() || n <= 0) return -1;
    unsigned int h = hashw((const u16*)k, (int)n);
    unsigned int slot = h & g_mask;
    for (int guard = 0; guard < 64; ++guard) {
        KeyEnt& e = g_tab[slot];
        if (!e.h) return -1;
        if (e.h == h) {
            const std::wstring& kk = g_keys[e.idx];
            if ((long long)kk.size() == n && memcmp(kk.c_str(), k, (size_t)n * 2) == 0) return e.idx;
        }
        slot = (slot + 1) & g_mask;
    }
    return -1;
}

// 富文本提示里有些句子是运行时拼出来的（后面接路径、数量等），
// 整串无法命中词典，这里按固定片段做子串替换，保留动态部分。
struct RepEnt { const wchar_t* en; const wchar_t* zh; };
static const RepEnt g_rep[] = {
    { L"<p>Create new folder on:",
      L"<p>在以下位置新建文件夹：" },
    { L"<p>The selected directory ",
      L"<p>所选目录 " },
    { L"<p>Invalid regular expression: ",
      L"<p>正则表达式无效：" },
    { L"<p>Unable to read the directory:</p><p>",
      L"<p>无法读取目录：</p><p>" },
    { L"<p>This action will remove a total of ",
      L"<p>此操作将移除总共 " },
    { L" item(s).</p><p><b>Deleted files will be removed permanently.</b><br/>They will not be moved to a recycle bin or sent to a trash can.</p><p>To delete these files, please press the <b>Ctrl</b> key while you click the Delete button below.</p>",
      L" 个项目。</p><p><b>删除的文件将被永久移除。</b><br/>不会移入回收站或废纸篓。</p><p>要删除这些文件，请在下方点击“删除”按钮时按住 <b>Ctrl</b> 键。</p>" },
    // Gaia 星表：动态统计提示（数字部分原样保留）
    { L"Standard error of proper motion in right ascension", L"赤经方向自行标准误差" },
    { L"Standard error of proper motion in declination",     L"赤纬方向自行标准误差" },
    { L"Standard error of right ascension",                  L"赤经标准误差" },
    { L"Standard error of declination",                      L"赤纬标准误差" },
    { L"Standard error of parallax",                         L"视差标准误差" },
    { L"Error on the integrated BP mean flux",               L"BP 集成平均流量误差" },
    { L"Error on the integrated RP mean flux",               L"RP 集成平均流量误差" },
    { L"Error on G-band mean flux",                          L"G 波段平均流量误差" },
    { L" in the range ", L" 在区间 " },
    { L" mas/year.", L" 毫角秒/年。" },
    { L" mas.", L" 毫角秒。" },
    { L" e-/s.", L" 电子/秒。" },
    // Gaia 星表：静态长句（含多段 tooltip 的逐段替换）
    { L"You can choose among Gaia Data Release 2 (Gaia DR2), Gaia Early Data Release 3 (EDR3), and Gaia Data Release 3 (DR3). The DR3/SP variant includes BP/RP sampled mean spectrum data for a subset of Gaia DR3 stars.",
      L"可在 Gaia Data Release 2 (Gaia DR2)、Gaia Early Data Release 3 (EDR3) 和 Gaia Data Release 3 (DR3) 之间选择。<br/>DR3/SP 变体包含 Gaia DR3 部分恒星的 BP/RP 采样平均光谱数据。" },
    { L"All catalogs are available in XPSD format and are fully supported by this implementation.",
      L"所有星表均以 XPSD 格式提供，并由本实现完整支持。" },
    { L"Search radius in degrees. Only sources at angular distances smaller than or equal to this value will be included in the search result.",
      L"搜索半径（度）。<br/>只有角距离小于或等于此值的天体才会包含在搜索结果中。" },
    { L"You can write an explicit angular distance as a space-separated list of degrees, arcminutes and arcseconds. The arcminutes and arcseconds items are optional, and you can specify a fractional part for the last item.",
      L"可以把显式角距离写成以空格分隔的度、角分、角秒列表。<br/>角分和角秒项可省略，最后一项可带小数部分。" },
    { L"If enabled, coordinates and angular distances will be shown as compound angular quantities (degrees/hours, (arc)minutes, and (arc)seconds). If disabled, coordinates will be shown as scalars (degrees or hours with decimals).",
      L"启用后，坐标和角距离以复合角度量显示（度/时、(角)分、(角)秒）。<br/>禁用时坐标以标量显示（带小数的度或时）。" },
    { L"When enabled, the right ascension coordinate will be shown expressed in hours, minutes, and seconds. When disabled, the right ascension will be expressed in degrees, arcminutes and arcseconds.",
      L"启用后，赤经坐标以时、分、秒显示；禁用时以度、角分、角秒显示。" },
    { L"Lower bound of the search magnitude range. Only sources with G magnitudes greater than this value will be included in the search result.",
      L"搜索星等范围下限。<br/>只有 G 星等大于此值的天体才会包含在搜索结果中。" },
    { L"Upper bound of the search magnitude range. Only sources with G magnitudes smaller than or equal to this value will be included in the search result.",
      L"搜索星等范围上限。<br/>只有 G 星等小于或等于此值的天体才会包含在搜索结果中。" },
    { L"The maximum number of sources that can be included in the search result.",
      L"搜索结果可包含的天体数量上限。" },
    { L"This is the data item used to sort output point sources in ascending order.",
      L"用于按升序排列输出点源的数据项。" },
    { L"When enabled, the search operation will generate an output plain text file including available data for all sources fulfilling the specified search criteria.",
      L"启用后，搜索操作将生成纯文本输出文件，包含满足搜索条件的所有天体的可用数据。" },
    { L"The format of generated output text files.", L"生成的输出文本文件的格式。" },
    { L"CSV will generate standard comma-separated files (CSV format).", L"CSV 将生成标准逗号分隔文件（CSV 格式）。" },
    { L"Tabular formats can be selected to write coordinates either as scalars in degrees, or using compound angular representations where right ascension is represented as separate hours, minutes and seconds, and declination as degrees, arcminutes and arcseconds.",
      L"可选择表格格式：坐标可写成标量度数，或使用复合角度表示——赤经分为时、分、秒，赤纬分为度、角分、角秒。" },
    { L"In all cases output text files can contain optional headers, which you can control with the", L"任何情况下，输出文本文件都可包含可选表头，由下方的" },
    { L"<p>The active image has no valid astrometric solution: ",
      L"<p>\u6d3b\u52a8\u56fe\u50cf\u6ca1\u6709\u6709\u6548\u7684\u5929\u4f53\u6d4b\u91cf\u89e3\uff1a" },
    { L"Text headers", L"文本表头" },
    { L"option below.", L"选项控制。" },

    // ===== GHS（GeneralisedHyperbolicStretch）脚本参数 tooltip =====
    // 这些说明在 DialogGHSMain.js 里由「字符串片段 + 运行期变量」拼成，
    // 例如 "... must be greater than or equal to " + name_LP + " and less than ..."。
    // 变量值是已汉化的参数名（「阴影保护点 (LP)」），所以整串永远不可能入库，
    // 只能把这些固定片段逐一替换。按长度降序排列，避免短片段先命中破坏长片段。
    { L". Note this parameter is specified by reference to the target image pixel values.</p>",
      L"。注意此参数以目标图像的像素值为参考。</p>" },
    { L". Note if LP is not zero, it will also be set to zero to ensure LP is not greater than SP.",
      L"。注意若 LP 不为零，也会被设为零，以确保 LP 不大于 SP。" },
    { L" by changing the form of the transform iteself. For concentrated stretches (such as initial stretches on linear images)",
      L" 聚焦的紧密程度，其方式是改变变换本身的形式。对于集中型拉伸（例如对线性图像做的初次拉伸），" },
    { L" a stretch within a histogram peak while de-focusing the stretch away from the histogram peak (such as bright stars).",
      L" 直方图峰值之内，同时在远离直方图峰值处（如亮星）降低聚焦。" },
    { L" focus point, SP.  By contrast, lower and -ve values of b tend to shift the histogram to a brighter (or dimmer) position without affecting its width too greatly.",
      L" 焦点 SP 展得更宽。相反，较小及为负的 b 值倾向于把直方图整体平移到更亮（或更暗）的位置，而不太影响其宽度。" },
    { L" contrast and brightness more evenly.  Large positive values of 'b' can be thought of as a histogram widener, ie spreading the histogram wider about the",
      L" 对比度与亮度分布得更加均匀。可将较大的正 b 值理解为直方图展宽器，即把直方图围绕" },
    { L" For adjustment of non-linear images, lower or -ve b (and/or lower D) parameters should be employed to distribute",
      L" 调整非线性图像时，应采用较小或为负的 b（和/或较小的 D）参数，使" },
    { L" As a general rule, the level of b employed will decrease as a stretch sequence nears completion, although",
      L" 一般规律是，随着拉伸序列接近完成，所用的 b 会逐渐减小；不过" },
    { L" larger +ve b values (with small D) can still be employed for precise placement of additional contrast.</p>",
      L" 仍可使用较大的正 b 值（配合较小的 D）来精确放置额外的对比度。</p>" },
    { L" a large +ve b factor should be employed to focus",
      L" 应采用较大的正 b 值，使拉伸聚焦在" },
    { L"<p>Controls how tightly focused the stretch is around ",
      L"<p>控制拉伸围绕 " },

    { L"contrast will be distributed symmetrically about SP.  While 'b' provides the degree of focus of the stretch,",
      L"对比度将围绕 SP 对称分布。'b' 提供拉伸的聚焦程度，" },
    { L" SP determines where that focus is applied.  SP should generally be placed within a histogram peak so that the stretch ",
      L" 而 SP 决定聚焦施加在何处。SP 通常应置于直方图峰值之内，使拉伸" },
    { L" will widen and lower the peak by adding the most contrast in the stretch at that point.  Pixel values will move away from",
      L" 在该处加入最多对比度，从而拓宽并压低峰值。像素值将离开" },
    { L"This parameter must be greater than or equal to ",
      L"此参数必须大于或等于 " },
    { L"This parameter must be less than or equal to 1 and not less than ",
      L"此参数必须小于或等于 1，且不小于 " },
    { L"This parameter must be greater than or equal to 0 and not greater than ",
      L"此参数必须大于或等于 0，且不大于 " },
    { L"<p>Sets the focus point around which the stretch is applied - ",
      L"<p>设置拉伸所围绕的焦点——" },
    { L"Symmetry point (SP) to ", L"对称点 (SP) 改为 " },

    { L"This is done by performing a linear stretch of the data below the 'LP' level by reserving contrast from the rest of the image.",
      L"做法是对 'LP' 电平以下的数据执行线性拉伸，其对比度取自图像的其余部分。" },
    { L"This is done by performing a linear stretch of the data above the 'HP' level by reserving contrast from the rest of the image.",
      L"做法是对 'HP' 电平以上的数据执行线性拉伸，其对比度取自图像的其余部分。" },
    { L" Moving the LP level towards the current setting of SP changes both the scope (range) and the amount of this contrast reservation, the net effect",
      L" 把 LP 电平移向当前 SP 设定，会同时改变该对比度保留的范围与数量，净效果是" },
    { L" Moving the HP level towards the current setting of SP increases both the scope (range) and the amount of this contrast reservation, the net effect",
      L" 把 HP 电平移向当前 SP 设定，会同时增大该对比度保留的范围与数量，净效果是" },
    { L" is to push the overal stretch to higher brightness levels while keeping the contrast and definition in the background.  The amount of",
      L" 把整体拉伸推向更高的亮度水平，同时保持背景的对比度与层次。" },
    { L" is to push the overal stretch to lower brightness levels while keeping the contrast and definition in the highlights.  The amount of",
      L" 把整体拉伸推向更低的亮度水平，同时保持高光的对比度与层次。" },
    { L" contrast reserved for the lowlights is such that the continuity of the stretch is preserved.  ",
      L" 为暗部保留的对比度会确保拉伸的连续性不被破坏。  " },
    { L" contrast reserved for the highlights is such that the continuity of the stretch is preserved.  ",
      L" 为高光保留的对比度会确保拉伸的连续性不被破坏。  " },
    { L"<p>Sets a value below which the stretch is modified to preserve contrast in the shadows/lowlights. ",
      L"<p>设置一个下限值，低于该值时拉伸会被调整，以保留阴影/暗部的对比度。" },
    { L"<p>Sets a value above which the stretch is modified to preserve contrast in the highlights/stars. ",
      L"<p>设置一个上限值，高于该值时拉伸会被调整，以保留高光/星点的对比度。" },

    { L" in such a way that LCP is the maximum fraction of pixels clipped in any channel and set to zero in the linear stretch.",
      L"，使 LCP 等于线性拉伸中任一通道被截断并置零的像素所占的最大比例。" },
    { L" in such a way that HCP is the maximum fraction of pixels clipped in any channel and set to 1.0 in the linear stretch.",
      L"，使 HCP 等于线性拉伸中任一通道被截断并置为 1.0 的像素所占的最大比例。" },
    { L"  Contrast gained by performing the linear stretch will be evenly distributed over the image, which will be dimmed.  Pixels with values less than the blackpoint will appear black and have 0 value.</p>",
      L" 线性拉伸获得的对比度将均匀分布到整幅图像上，图像会变暗。数值低于黑点的像素显示为黑色，值为 0。</p>" },
    { L"  Contrast gained by performing the linear stretch will be evenly distributed over the image, which will be brightened.  Pixels with values greater than the white-point will appear white and have a value of 1.0.</p>",
      L" 线性拉伸获得的对比度将均匀分布到整幅图像上，图像会变亮。数值高于白点的像素显示为白色，值为 1.0。</p>" },
    { L"  Note that any pixel with values less than the black-point input will be clipped and the data lost.",
      L" 注意任何数值低于黑点输入的像素都会被截断，数据随之丢失。" },
    { L"  Note that any pixel with value greater than the white-point input will be clipped and the data lost.",
      L" 注意任何数值高于白点输入的像素都会被截断，数据随之丢失。" },
    { L"<p>Sets the clipping level for linear stretch of the image.",
      L"<p>设置图像线性拉伸的截断电平。" },
    { L"<p>Sets the upper clipping level for linear stretch of the image.",
      L"<p>设置图像线性拉伸的上限截断电平。" },
    { L" Updating this parameter will automatically update ",
      L" 更新此参数会自动更新 " },
    { L"<p>Reset ", L"<p>重置 " },
    // 动态读数与历史按钮（计数会变，只能按前缀片段替换）
    { L"K Value: ", L"K 值: " },
    { L"Undo (", L"撤销 (" },
    { L"Redo (", L"重做 (" },
};
static const int NREP = (int)(sizeof(g_rep) / sizeof(g_rep[0]));

// ---------- 动态串处理 ----------
// 界面上有些文本是运行时拼接出来的，词典精确匹配不到：
//   "Undo: <Unavailable>"  "Redo: <Unavailable>"  "Paste: <Unavailable>" ...
// 这里按前缀/占位符做规则替换，结果同样走内容寻址缓存。
// 拼接型 tooltip 的「标签边界后缀」回退。
// 脚本描述类 tooltip 由运行期拼成：<b>脚本名 v1.0</b> &mdash; 描述…
// 前缀（名称/版本）是变量，整串永远不可能入库，词典里只能存「描述段」。
// 这里在每个标签边界、破折号之后尝试用后半段查词典；命中则保留前缀原样，
// 只替换后半段。对 </p><p>… 这类残片同样有效。
static bool suffix_hit(const std::wstring& r, std::wstring& out) {
    if (r.size() < 32) return false;
    bool hasTag  = r.find(L'<') != std::wstring::npos;
    bool hasDash = r.find(L"&mdash;") != std::wstring::npos
                || r.find(L"\u2014") != std::wstring::npos;
    if (!hasTag && !hasDash) return false;

    std::vector<size_t> cand;
    cand.reserve(48);
    size_t pos = 0;
    for (int guard = 0; guard < 48; ++guard) {
        size_t m = std::wstring::npos, t;
        if ((t = r.find(L'>', pos)) != std::wstring::npos) m = t + 1;
        if ((t = r.find(L"&mdash;", pos)) != std::wstring::npos && t + 7 < m) m = t + 7;
        if ((t = r.find(L"\u2014", pos)) != std::wstring::npos && t + 1 < m) m = t + 1;
        if ((t = r.find(L"&nbsp;", pos)) != std::wstring::npos && t + 6 < m) m = t + 6;
        if (m == std::wstring::npos || m >= r.size()) break;
        cand.push_back(m);
        pos = m;
    }

    for (size_t i = 0; i < cand.size(); ++i) {
        size_t a = cand[i];
        while (a < r.size() && (r[a] == L' ' || r[a] == L'\t' || r[a] == L'\r' || r[a] == L'\n')) ++a;
        if (r.size() < a + 24) continue;
        int idx = findkey(r.c_str() + a, (long long)(r.size() - a));
        if (idx < 0) continue;
        const std::string& u8 = g_utf8[idx];
        int wn = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), 0, 0);
        if (wn <= 0) continue;
        std::wstring zh;
        zh.resize((size_t)wn);
        MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &zh[0], wn);
        out = r.substr(0, a) + zh;
        return true;
    }
    return false;
}

static void* dyn(const u16* p, long long n) {
    bool undo = (n > 6 && p[0] == 'U' && p[1] == 'n' && p[2] == 'd' && p[3] == 'o' && p[4] == ':' && p[5] == ' ');
    bool redo = (n > 6 && p[0] == 'R' && p[1] == 'e' && p[2] == 'd' && p[3] == 'o' && p[4] == ':' && p[5] == ' ');
    // "Reading icon: <标识符>" —— 加载工作流时的控制台输出
    bool rdicon = (n > 14 && memcmp(p, L"Reading icon: ", 14 * 2) == 0);
    static const wchar_t* kLoaded = L" icon(s) loaded.";
    size_t klen = wcslen(kLoaded);
    bool loaded = (n > (long long)klen + 1 && memcmp(p + n - klen, kLoaded, klen * 2) == 0);
    bool lt = false;
    for (long long i = 0; i < n; ++i) { if (p[i] == '<') { lt = true; break; } }
    bool br = (n > 3 && p[n - 1] == ']');
    // Gaia 星表的动态统计提示："Standard error of ... < 0.13 mas." 等，数字部分运行时拼接
    bool se = (n > 21 && memcmp(p, L"Standard error of ", 17 * 2) == 0)
           || (n > 12 && memcmp(p, L"Error on ", 9 * 2) == 0);
    // 关键修复：早期版本只在「含 HTML 标签 / 带快捷键后缀 / Standard error 前缀」时才走
    // 动态替换，而 Gaia 星表等进程的参数 tooltip 是**纯文本**（无 <p> 标签），
    // 于是全部被这行提前 return 挡掉，界面上一直是英文，且不会写 tip_raw.txt
    //（因为提交流程要求 lt=true）。改为：只要长度够，就一定要尝试 g_rep 固定片段替换。
    bool rep = (n >= 12);
    // 关键修复：n>=12 的门禁把「39 items」「6248 px」「72.00 px/inch」这类 7~11 字符的
    // 计数/单位串整条挡在门外，状态栏和尺寸读数一直是英文。放宽到 4 字符。
    bool cnt = (n >= 4);
    if (!undo && !redo && !rdicon && !loaded && !lt && !br && !se && !rep && !cnt) return 0;

    std::wstring w((const wchar_t*)p, (size_t)n);
    std::wstring r = w;
    if (undo)      { r = L"\u64a4\u9500\uff1a"; r += w.substr(6); }
    else if (redo) { r = L"\u91cd\u505a\uff1a"; r += w.substr(6); }
    else if (rdicon) { r = L"\u8bfb\u53d6\u56fe\u6807\uff1a"; r += w.substr(14); }
    else if (loaded) {
        r = w.substr(0, (size_t)n - klen);
        r += L" \u4e2a\u56fe\u6807\u5df2\u52a0\u8f7d\u3002";
    }

    // 拼接型 tooltip（脚本描述等）：只替换破折号/标签之后的后半段，前缀保留
    {
        std::wstring o;
        if (suffix_hit(r, o)) r = o;
    }

    // 含真实换行的长帮助文本：词典按行存储，换行只能写成字面 "\n"。
    // 整串查不到时，把真实换行等价替换为字面 \n 再查一次。
    if (r.find(L'\n') != std::wstring::npos || r.find(L'\r') != std::wstring::npos) {
        std::wstring e;
        e.reserve(r.size() + 16);
        for (size_t i = 0; i < r.size(); ++i) {
            wchar_t c = r[i];
            if (c == L'\r') continue;
            if (c == L'\n') { e += L'\\'; e += L'n'; }
            else e += c;
        }
        int idx = e.empty() ? -1 : findkey(e.c_str(), (long long)e.size());
        if (idx >= 0) {
            const std::string& u8 = g_utf8[idx];
            int wn = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), 0, 0);
            if (wn > 0) {
                std::wstring zh;
                zh.resize((size_t)wn);
                MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &zh[0], wn);
                r = zh;
            }
        }
    }

    // 形如 "Readout Mode [Alt+R]" 的快捷键后缀串：拆出名称查词典再拼回
    if (w.size() > 3 && w[w.size() - 1] == ']') {
        size_t lb = w.rfind(L" [");
        if (lb != std::wstring::npos && lb > 0) {
            int idx = findkey(w.c_str(), (long long)lb);
            if (idx >= 0) {
                std::wstring zh;
                const std::string& u8 = g_utf8[idx];
                int wn = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), 0, 0);
                if (wn > 0) {
                    zh.resize((size_t)wn);
                    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &zh[0], wn);
                    r = zh + w.substr(lb);
                }
            }
        }
    }
    // "Zoom 1:2" / "Zoom 2:1 [Ctrl+2]" 这类按比例生成的提示
    if (r.size() > 5 && r.compare(0, 5, L"Zoom ") == 0 && r[5] >= '0' && r[5] <= '9')
        r = std::wstring(L"\u7f29\u653e ") + r.substr(5);

    // "Detail Layer N/M"（DynamicPSF 等细节层标题）→ 细节层 N/M
    if (r.size() > 13 && r.compare(0, 13, L"Detail Layer ") == 0
        && r[13] >= L'0' && r[13] <= L'9') {
        size_t sl = r.find(L'/', 13);
        if (sl != std::wstring::npos && sl + 1 < r.size() && r[sl + 1] >= L'0' && r[sl + 1] <= L'9')
            r = std::wstring(L"\u7ec6\u8282\u5c42 ") + r.substr(13);
    }
    // "Frames (N)"（SubframeSelector 帧计数标签）→ 帧 (N)
    if (r.size() > 9 && r.compare(0, 8, L"Frames (") == 0) {
        size_t cp = r.find(L')', 8);
        if (cp != std::wstring::npos && cp > 8) {
            bool alldig = true;
            for (size_t i = 8; i < cp; ++i)
                if (r[i] < L'0' || r[i] > L'9') { alldig = false; break; }
            if (alldig) r = std::wstring(L"\u5e27 (") + r.substr(8);
        }
    }
    // "Series N..."（图形样式/图例）→ 序列 N...
    if (r.size() > 7 && r.compare(0, 7, L"Series ") == 0
        && r[7] >= L'0' && r[7] <= L'9')
        r = std::wstring(L"\u5e8f\u5217 ") + r.substr(7);
    // "Gaussian (N)"（小波缩放函数下拉项）→ 高斯 (N)
    if (r.size() > 11 && r.compare(0, 9, L"Gaussian (") == 0) {
        size_t cp = r.find(L')', 10);
        if (cp != std::wstring::npos && cp > 10) {
            bool alldig = true;
            for (size_t i = 10; i < cp; ++i)
                if (r[i] < L'0' || r[i] > L'9') { alldig = false; break; }
            if (alldig) r = std::wstring(L"\u9ad8\u65af (") + r.substr(10);
        }
    }

    // 很多 QComboBox 条目带前导空格（如 " Format 1 of 22"、" Add/Edit Point | ..."），
    // 直接按前缀比较会全部漏掉，这里先剥掉首尾空白，规则处理完再拼回。
    std::wstring padL, padR;
    {
        size_t a = 0, b = r.size();
        while (a < b && (r[a] == L' ' || r[a] == L'\t')) ++a;
        while (b > a && (r[b - 1] == L' ' || r[b - 1] == L'\t')) --b;
        if (a > 0 || b < r.size()) {
            padL = r.substr(0, a);
            padR = r.substr(b);
            r = r.substr(a, b - a);
        }
    }

    // "Readout | x=... | y=..."（图像视图状态栏坐标读数）
    if (r.size() > 10 && r.compare(0, 10, L"Readout | ") == 0)
        r = std::wstring(L"\u8bfb\u6570 | ") + r.substr(10);
    // CurvesTransformation 的曲线编辑提示："Add/Edit Point | x = ... | y = ..."
    if (r.size() > 17 && r.compare(0, 17, L"Add/Edit Point | ") == 0)
        r = std::wstring(L"\u6dfb\u52a0/\u7f16\u8f91\u70b9 | ") + r.substr(17);
    if (r.size() > 15 && r.compare(0, 15, L"Delete Point | ") == 0)
        r = std::wstring(L"\u5220\u9664\u70b9 | ") + r.substr(15);
    if (r.size() > 10 && r.compare(0, 10, L"Zoom In | ") == 0)
        r = std::wstring(L"\u653e\u5927 | ") + r.substr(10);
    // "Category N of M" / "Format N of M"（格式转换器等下拉分组计数）
    for (int kk = 0; kk < 2; ++kk) {
        const wchar_t* pre = kk ? L"Format " : L"Category ";
        size_t pl = kk ? 7 : 9;
        if (r.size() > pl + 4 && r.compare(0, pl, pre) == 0) {
            size_t i = pl;
            while (i < r.size() && r[i] >= L'0' && r[i] <= L'9') ++i;
            if (i > pl && i + 4 < r.size() && r.compare(i, 4, L" of ") == 0) {
                size_t j = i + 4;
                while (j < r.size() && r[j] >= L'0' && r[j] <= L'9') ++j;
                if (j > i + 4 && j == r.size())
                    r = std::wstring(kk ? L"\u683c\u5f0f " : L"\u7c7b\u522b ")
                        + r.substr(pl, i - pl) + L" / " + r.substr(i + 4);
            }
        }
    }
    // "Custom (255, 64, 64)"（颜色按钮的自定义色提示）→ 自定义 (255, 64, 64)
    if (r.size() > 8 && r.compare(0, 7, L"Custom ") == 0 && r[7] == '(' && r[r.size() - 1] == ')') {
        bool okc = true;
        for (size_t i = 8; i + 1 < r.size(); ++i) {
            wchar_t c = r[i];
            if (!((c >= L'0' && c <= L'9') || c == L',' || c == L' ')) { okc = false; break; }
        }
        if (okc) r = std::wstring(L"\u81ea\u5b9a\u4e49 ") + r.substr(7);
    }
    // "18 items" / "1 pixel(s)" / "3  (9 elements)" 这类纯计数串
    {
        size_t i = 0;
        while (i < r.size() && r[i] >= L'0' && r[i] <= L'9') ++i;
        if (i > 0 && i < r.size()) {
            std::wstring tail = r.substr(i);
            if (tail == L" items" || tail == L" item")
                r = r.substr(0, i) + L" \u9879";
            else if (tail == L" pixel(s)" || tail == L" pixels" || tail == L" pixel")
                r = r.substr(0, i) + L" \u50cf\u7d20";
            else if (tail.size() > 10 && tail.compare(tail.size() - 10, 10, L" elements)") == 0)
                r = r.substr(0, i) + tail.substr(0, tail.size() - 10) + L"\u4e2a\u5143\u7d20)";
        }
    }
    // "Qty: 0, Level: 1.0, Sigma: 50.0"（反卷积/小波参数摘要）
    if (r.size() > 5 && r.compare(0, 5, L"Qty: ") == 0) {
        size_t q;
        r = std::wstring(L"\u6570\u91cf\uff1a") + r.substr(5);
        while ((q = r.find(L", Level: ")) != std::wstring::npos) r.replace(q, 9, L"\uff0c\u5c42\u7ea7\uff1a");
        while ((q = r.find(L", Sigma: ")) != std::wstring::npos) r.replace(q, 9, L"\uff0cSigma\uff1a");
    }
    // "Kernel size: 7"（形态学/卷积核尺寸动态标签）
    if (r.size() > 13 && r.compare(0, 13, L"Kernel size: ") == 0
        && r[13] >= L'0' && r[13] <= L'9')
        r = std::wstring(L"\u6838\u5927\u5c0f\uff1a") + r.substr(13);
    // "Automatic (AMD Radeon RX 580 2048SP)"（显卡自动选择项）
    if (r.size() > 12 && r.compare(0, 11, L"Automatic (") == 0 && r[r.size() - 1] == ')')
        r = std::wstring(L"\u81ea\u52a8\uff08") + r.substr(11);
    // "6248 px" / "72.00 px/inch"（像素尺寸读数）
    {
        size_t i = 0;
        while (i < r.size() && ((r[i] >= L'0' && r[i] <= L'9') || r[i] == L'.')) ++i;
        if (i > 0 && i < r.size()) {
            std::wstring tail = r.substr(i);
            if (tail == L" px")
                r = r.substr(0, i) + L" \u50cf\u7d20";
            else if (tail == L" px/inch")
                r = r.substr(0, i) + L" \u50cf\u7d20/\u82f1\u5bf8";
            else if (tail.size() > 8 && tail.compare(tail.size() - 7, 7, L" bytes)") == 0)
                r = r.substr(0, i) + tail.substr(0, tail.size() - 7) + L"\u5b57\u8282)";
        }
    }
    // "w:6248 · h:4176 · n:3 · i16 · RGB · 149.298 MiB"（图像几何摘要）
    if (r.size() > 12 && r.compare(0, 2, L"w:") == 0 && r.find(L" \u00b7 h:") != std::wstring::npos) {
        r = std::wstring(L"\u5bbd:") + r.substr(2);
        size_t q;
        if ((q = r.find(L" \u00b7 h:")) != std::wstring::npos) r.replace(q, 5, L" \u00b7 \u9ad8:");
        if ((q = r.find(L" \u00b7 n:")) != std::wstring::npos) r.replace(q, 5, L" \u00b7 \u901a\u9053:");
    }
    // "row:0 col:1 on"（卷积核/矩阵单元状态）
    if (r.size() > 4 && r.compare(0, 4, L"row:") == 0) {
        size_t q;
        r = std::wstring(L"\u884c:") + r.substr(4);
        if ((q = r.find(L" col:")) != std::wstring::npos) r.replace(q, 5, L" \u5217:");
        if (r.size() > 3 && r.compare(r.size() - 3, 3, L" on") == 0)
            r.replace(r.size() - 3, 3, L" \u5f00");
        else if (r.size() > 4 && r.compare(r.size() - 4, 4, L" off") == 0)
            r.replace(r.size() - 4, 4, L" \u5173");
    }

    // "You have N PixInsight updates"（启动横幅/更新提示）
    if (r.size() > 26 && r.compare(0, 9, L"You have ") == 0) {
        size_t p = r.find(L" PixInsight update");
        if (p != std::wstring::npos && p > 9) {
            std::wstring num = r.substr(9, p - 9);
            bool alldig = true;
            for (size_t i = 0; i < num.size(); ++i)
                if (num[i] < L'0' || num[i] > L'9') { alldig = false; break; }
            if (alldig) r = std::wstring(L"\u60a8\u6709 ") + num + L" \u4e2a PixInsight \u66f4\u65b0";
        }
    }

    // "Purge cache (0.05 KiB)"（WBPP 缓存清理按钮）
    if (r.size() > 13 && r.compare(0, 12, L"Purge cache (") == 0 && r[r.size() - 1] == ')')
        r = std::wstring(L"\u6e05\u9664\u7f13\u5b58\uff08") + r.substr(12);

    // "3 message(s):"（诊断消息计数标题）
    {
        size_t i = 0;
        while (i < r.size() && r[i] >= L'0' && r[i] <= L'9') ++i;
        if (i > 0 && i < r.size() && r.substr(i) == L" message(s):")
            r = r.substr(0, i) + L" \u6761\u6d88\u606f\uff1a";
    }

    if (!padL.empty() || !padR.empty()) r = padL + r + padR;

    size_t at;
    while ((at = r.find(L"<Unavailable>")) != std::wstring::npos) r.replace(at, 13, L"<\u4e0d\u53ef\u7528>");
    while ((at = r.find(L"<No Entries>"))  != std::wstring::npos) r.replace(at, 12, L"<\u65e0\u6761\u76ee>");

    // 富文本里由运行时拼接的句子：按固定片段替换，保留路径/数量等动态部分
    for (int i = 0; i < NREP; ++i) {
        size_t at2;
        size_t len = wcslen(g_rep[i].en);
        while ((at2 = r.find(g_rep[i].en)) != std::wstring::npos)
            r.replace(at2, len, g_rep[i].zh);
    }

    // 通用富文本处理：工具提示/帮助文字是 HTML（<i>/<b>/<p> 等标签包裹）。
    // 先尝试「剥掉全部标签后的整句」查词典（词典存无标签纯文本），
    // 命中则整句替换；未命中再逐段处理，保留 HTML 结构与动态部分。
    if (lt && !g_keys.empty()) {
        std::wstring plain;
        norm_tip(r, plain);
        bool whole = false;
        int idx0 = plain.empty() ? -1 : findkey(plain.c_str(), (long long)plain.size());
        if (idx0 < 0 && !plain.empty()) {
            std::wstring packed = plain;
            compact_ops(packed);
            if (!packed.empty())
                idx0 = findkey(packed.c_str(), (long long)packed.size());
        }
        if (idx0 >= 0) {
            const std::string& u8 = g_utf8[idx0];
            int wn = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), 0, 0);
            if (wn > 0) {
                std::wstring zh;
                zh.resize((size_t)wn);
                MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &zh[0], wn);
                r = zh;
                whole = true;
            }
        }
        if (!whole) {
        std::wstring out;
        out.reserve(r.size() + 32);
        size_t i = 0, segStart = 0;
        bool inTag = false;
        while (i <= r.size()) {
            if (i == r.size() || r[i] == '<') {
                // 处理 [segStart, i) 这段纯文本
                if (i > segStart) {
                    size_t len = i - segStart;
                    // 去掉首尾空白，得到可查词典的连续文本
                    size_t a = segStart, b = i;
                    while (a < b && (r[a] == ' ' || r[a] == '\t')) ++a;
                    while (b > a && (r[b - 1] == ' ' || r[b - 1] == '\t')) --b;
                    if (b > a) {
                        int idx = findkey(r.c_str() + a, (long long)(b - a));
                        if (idx >= 0) {
                            const std::string& u8 = g_utf8[idx];
                            int wn = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), 0, 0);
                            if (wn > 0) {
                                std::wstring zh;
                                zh.resize((size_t)wn);
                                MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &zh[0], wn);
                                out.append(r, segStart, a - segStart);
                                out += zh;
                                out.append(r, b, i - b);
                                segStart = i;
                                continue;  // 不跳过 r[i]=='<'，下一轮按标签处理
                            }
                        }
                    }
                }
                if (i == r.size()) break;
                // 找到标签结束
                inTag = true;
                ++i;
                while (i < r.size() && r[i] != '>') ++i;
                if (i < r.size()) ++i; // 跳过 '>'
                segStart = i;
                continue;
            }
            ++i;
        }
        if (segStart < r.size()) out.append(r, segStart, r.size() - segStart);
        if (out != r) r = out;
        }
        if (!whole && r == w) {
            tiplog(w, plain);
            if (g_probe) {
                std::wstring pv = plain.size() > 1600 ? plain.substr(0, 1600) : plain;
                prwrite(std::wstring(L"### NORM len=") + std::to_wstring(plain.size()) + L" ###\r\n" +
                        pv + L"\r\n");
            }
        }
    }

    if (r == w) { if (!lt) plainlog(w); return 0; }

    if (g_diag) anylog(r, "REPLACED");

    char u8[32768];
    int cn = WideCharToMultiByte(CP_UTF8, 0, r.c_str(), (int)r.size(), u8, (int)sizeof(u8) - 1, 0, 0);
    if (cn <= 0) return 0;
    std::string out8(u8, (size_t)cn);
    if (lt) out8 = wrap_tip(out8);          // 富文本长提示按句读折行
    return qcache_str(out8);
}

// ---------- 分段组合翻译 ----------
// 部分长参数说明（BXT / SXT / StarAlignment / WBPP 等）在内存里按 <p>…</p>
// 分段存放，运行时再拼成一整串；也有部分串在内存分块边界被切断，整串永远
// 不可能与词典键相等。这两种情况下整串查表必然失败，但每一段单独存在。
// 这里按 </p> 逐段查表：全部含拉丁字母的段都命中才拼接返回，否则放弃。
static void* compose_paras(const u16* p, long long n) {
    if (n < 120) return 0;
    std::wstring w((const wchar_t*)p, (size_t)n);
    if (w.find(L"</p>") == std::wstring::npos) return 0;

    std::wstring out;
    out.reserve(w.size() * 2);
    size_t pos = 0;
    int segs = 0, hits = 0;
    while (pos < w.size()) {
        size_t e = w.find(L"</p>", pos);
        size_t nxt = (e == std::wstring::npos) ? w.size() : e + 4;
        std::wstring part = w.substr(pos, nxt - pos);
        pos = nxt;
        if (part.empty()) continue;
        ++segs;
        bool latin = false;
        for (size_t i = 0; i < part.size(); ++i) {
            wchar_t c = part[i];
            if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) { latin = true; break; }
        }
        if (!latin) { out += part; continue; }
        int idx = findkey(part.c_str(), (long long)part.size());
        if (idx < 0) {
            // 词典里有时只存不带外层 <p> 的正文
            size_t a = 0, b = part.size();
            if (b > 7 && part.compare(0, 3, L"<p>") == 0) a = 3;
            if (b > a + 4 && part.compare(b - 4, 4, L"</p>") == 0) b -= 4;
            if (a > 0 || b < part.size())
                idx = findkey(part.c_str() + a, (long long)(b - a));
        }
        if (idx < 0) return 0;
        const std::string& u8 = g_utf8[idx];
        int wn = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), 0, 0);
        if (wn <= 0) return 0;
        std::wstring zh;
        zh.resize((size_t)wn);
        MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &zh[0], wn);
        out += zh;
        ++hits;
    }
    if (segs < 2 || hits < 1) return 0;
    return qcache_str(w2u8(out));
}

// ---------- 翻译查询 ----------
static thread_local int g_tpath = 0;   // T() 的返回路径（诊断用）

// 长文本路径追踪：记录「这条长串是被哪个钩子看到的 + 走了 T() 的哪条返回路径」。
// pi_seen.txt 只有文本本身，看不出钩子来源，导致「词典命中了却还是英文」时
// 无法判断是查找失败还是上层没用返回值。仅当 PI_ZH_MISS=1 时生效。
struct TPathTrace {
    int          n;
    const u16*   p;
    const char*  hn;
    TPathTrace(int n_, const u16* p_) : n(n_), p(p_) { hn = g_hn; }
    ~TPathTrace() {
        if (!g_miss || n < 200) return;
        std::wstring head((const wchar_t*)p, (size_t)(n < 70 ? n : 70));
        EnterCriticalSection(&g_cs);
        static std::unordered_set<std::wstring> seen;
        std::wstring key = std::to_wstring(g_tpath) + L"|" + head;
        bool add = seen.insert(key).second && seen.size() <= 1200;
        LeaveCriticalSection(&g_cs);
        if (!add) return;
        wchar_t dir[MAX_PATH]; selfdir(dir);
        wcscat_s(dir, MAX_PATH, L"\\pi_trace.txt");
        std::wstring line = std::wstring(L"tpath=") + std::to_wstring(g_tpath) +
                            L" hook=" + std::wstring(hn, hn + strlen(hn)) +
                            L" len=" + std::to_wstring(n) +
                            L" head=" + head + L"\r\n";
        char u8[4096];
        int cn = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), u8, (int)sizeof(u8) - 2, 0, 0);
        if (cn <= 0) return;
        HANDLE h = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (h != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(h, u8, (DWORD)cn, &wr, 0); CloseHandle(h); }
    }
};

static inline const void* T(const void* s) {
    if (!s || !g_ready) { g_tpath = 1; return s; }
    InterlockedIncrement(&g_tcalls);
    long long n = q_size(s);
    // 上限 8000：色彩管理的「渲染意图」原文是 5 段 <p> 共 2042 字符，
    // 旧上限 2000 会把 2000 以上的长提示直接放行，词条再全也不生效。
    if (n <= 0 || n > 8000) { g_tpath = 2; return s; }
    const u16* p = q_utf16(s);
    if (!p) { g_tpath = 3; return s; }
    TPathTrace tr((int)n, p);
    if (g_diag && n >= 100) anylog(std::wstring((const wchar_t*)p, (size_t)n), g_hn);
    probecheck(p, n);
    if (g_seen) record(p, (int)n);
    if (g_keys.empty()) { g_tpath = 4; if (void* d = dyn(p, n)) return d; missrec(p, (int)n); return s; }

    // 富文本兜底：剥掉 <p>/<i>/<b> 等标记后整句查词典。
    // 独立于 dyn() 的门禁逻辑，只要串里含 '<' 就一定尝试，避免被上游条件挡住。
    bool hasTag = false;
    if (n >= 20) {
        for (long long i = 0; i < n; ++i) { if (p[i] == L'<') { hasTag = true; break; } }
        if (hasTag) {
            std::wstring w0((const wchar_t*)p, (size_t)n), pl;
            norm_tip(w0, pl);
            int id = pl.empty() ? -1 : findkey(pl.c_str(), (long long)pl.size());
            if (id < 0 && !pl.empty()) {
                std::wstring packed = pl;
                compact_ops(packed);
                if (!packed.empty()) id = findkey(packed.c_str(), (long long)packed.size());
            }
            if (g_probe && n >= 40) {
                std::wstring pv = pl.size() > 1500 ? pl.substr(0, 1500) : pl;
                prwrite(std::wstring(L"### NORM len=") + std::to_wstring(pl.size()) +
                        L" hit=" + (id >= 0 ? L"1" : L"0") + L" ###\r\n" + pv + L"\r\n");
            }
            if (id >= 0) {
                if (void* q = qcache_tip(id)) { g_tpath = 11; return q; }
            }
        }
    }

    // 拼接型对话框标题（Select <色彩空间> Channel #N Source Image）。
    // 放在片段替换之前：dyn() 可能把 "Select " 之类的前缀替换掉，导致标题永远匹配不上。
    if (n >= 28 && n <= 80 && p[0] == L'S') {
        std::wstring w((const wchar_t*)p, (size_t)n), o;
        if (title_pat(w, o)) {
            if (void* q = qcache_str(w2u8(o))) { g_tpath = 12; return q; }
        }
    }

    // 白平衡系数控件的拼接型提示（Red/Green/Blue channel correction factor）
    if (n >= 20 && n <= 120) {
        std::wstring w((const wchar_t*)p, (size_t)n), o;
        if (wb_tip_pat(w, o)) {
            if (void* q = qcache_str(w2u8(o))) { g_tpath = 14; return q; }
        }
    }

    // 尾部标点容错。必须放在哈希查找**之前**：
    // 这两个容错原本写在哈希循环后面，但「哈希槽为空」的分支在它之前就
    // return 了，所以带尾随空格的标签（"Denoise Color: "）从来走不到这里。
    //
    // 1) 尾部空白：部分下拉项/标签的字面量带尾随空格（"Same as source "），
    //    而词典键不含空格，哈希必然不中。
    // 2) 尾部冒号：RC-Astro 三件套（BlurXTerminator / NoiseXTerminator /
    //    StarXTerminator）等第三方进程的参数标签是「显示名 + ": "」形式
    //    （"Denoise Intensity: "），词典里只存显示名。这里剥掉尾部空白与
    //    冒号后重查，命中则把剥掉的后缀（" : " / ": "）原样拼回，
    //    既不影响排版，也不必为每个标签在词典里存两份。
    if (n > 1) {
        long long m = n;
        while (m > 0 && (p[m - 1] == L' ' || p[m - 1] == L'\t' || p[m - 1] == L'\r' || p[m - 1] == L'\n')) --m;
        if (m > 0 && m != n) {
            int id = findkey((const wchar_t*)p, m);
            if (id >= 0) {
                if (void* q = hasTag ? qcache_tip(id) : qcache(id)) { g_tpath = 13; return q; }
            }
        }
        // 冒号回退：仅当串尾确实是冒号（可带尾随空白）时生效
        long long c = m;
        while (c > 0 && (p[c - 1] == L':' || p[c - 1] == L'\uff1a')) --c;
        if (c > 0 && c != m) {
            long long k = c;
            while (k > 0 && (p[k - 1] == L' ' || p[k - 1] == L'\t')) --k;
            // k >= 2 避免把单字母（如 Tab 标签 "A:"）误当词条
            if (k >= 2) {
                int id = findkey((const wchar_t*)p, k);
                if (id >= 0) {
                    std::wstring zh;
                    const std::string& u8 = g_utf8[id];
                    int wn = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), 0, 0);
                    if (wn > 0) {
                        zh.resize((size_t)wn);
                        MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &zh[0], wn);
                    }
                    if (!zh.empty()) {
                        std::wstring suf((const wchar_t*)p + k, (size_t)(n - k));
                        if (void* q = qcache_str(w2u8(zh + suf))) { g_tpath = 15; return q; }
                    }
                }
            }
        }
    }

    // 助记符回退：Qt 标准菜单项与对话框按钮传的是带 & 的文本
    // （"&Cut" / "&Copy" / "&Paste" / "&Select All" / "&OK" / "&Cancel" /
    //   "&Yes" / "&No" / "&Save" / "&Open" / "&Close" / "&Float" / "&New Folder" 等），
    // 词典里存的是去掉 & 的显示名，哈希精确匹配必然不中——这是 Qt 自带
    // 右键菜单、QMessageBox、QDialogButtonBox 长期显示英文的原因。
    // 命中后把 & 补回译文首位，保留 Qt 的助记符下划线。
    if (n >= 2 && n <= 64) {
        bool has_amp = false;
        for (long long i = 0; i < n; ++i) {
            if (p[i] == L'&') { has_amp = true; break; }
        }
        if (has_amp) {
            std::wstring bare;
            bare.reserve((size_t)n);
            for (long long i = 0; i < n; ++i) {
                if (p[i] != L'&') bare.push_back((wchar_t)p[i]);
            }
            if (!bare.empty() && (long long)bare.size() < n) {
                int id = findkey(bare.c_str(), (long long)bare.size());
                if (id < 0) {
                    // 去 & 后再剥尾部空白与冒号，覆盖 "&Files of type: " 这类
                    long long m2 = (long long)bare.size();
                    while (m2 > 0 && (bare[(size_t)(m2 - 1)] == L' ' || bare[(size_t)(m2 - 1)] == L'\t')) --m2;
                    if (m2 > 0 && bare[(size_t)(m2 - 1)] == L':') {
                        while (m2 > 0 && bare[(size_t)(m2 - 1)] == L':') --m2;
                        while (m2 > 0 && (bare[(size_t)(m2 - 1)] == L' ' || bare[(size_t)(m2 - 1)] == L'\t')) --m2;
                    }
                    if (m2 >= 2 && m2 != (long long)bare.size()) {
                        id = findkey(bare.c_str(), m2);
                    }
                }
                if (id >= 0) {
                    std::wstring zh;
                    const std::string& u8 = g_utf8[id];
                    int wn = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), 0, 0);
                    if (wn > 0) {
                        zh.resize((size_t)wn);
                        MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &zh[0], wn);
                    }
                    if (!zh.empty()) {
                        if (p[0] == L'&' && zh[0] != L'&') zh.insert(zh.begin(), L'&');
                        if (void* q = qcache_str(w2u8(zh))) { g_tpath = 17; return q; }
                    }
                }
            }
        }
    }

    unsigned int h = hashw(p, (int)n);
    unsigned int slot = h & g_mask;
    for (int guard = 0; guard < 64; ++guard) {
        KeyEnt& e = g_tab[slot];
        if (!e.h) { g_tpath = 7; if (void* d = dyn(p, n)) return d; if (void* c = compose_paras(p, n)) { g_tpath = 16; return c; } missrec(p, (int)n); return s; }
        if (e.h == h) {
            const std::wstring& k = g_keys[e.idx];
            if ((long long)k.size() == n && memcmp(k.c_str(), p, (size_t)n * 2) == 0) {
                void* q = hasTag ? qcache_tip(e.idx) : qcache(e.idx);
                if (q) { g_tpath = 5; return q; }
                g_tpath = 6;
                if (void* d = dyn(p, n)) return d;   // 词典命中但译文无效时仍尝试片段替换
                return s;
            }
        }
        slot = (slot + 1) & g_mask;
    }
    if (void* d = dyn(p, n)) { g_tpath = 8; return d; }
    if (void* c = compose_paras(p, n)) { g_tpath = 16; return c; }
    missrec(p, (int)n);

    g_tpath = 9;
    return s;
}

// ---------- 钩子：完整转发全部参数 ----------
#define HOOK(RET, nm, PARAMS, ARGS)                     \
    typedef RET (__cdecl* T_##nm) PARAMS;               \
    static T_##nm o_##nm = 0;                           \
    static RET __cdecl h_##nm PARAMS { g_hn = #nm; return o_##nm ARGS; }

HOOK(void, w_setWindowTitle, (VP a, const void* s),                    (a, T(s)))
HOOK(void, w_setToolTip,     (VP a, const void* s),                    (a, T(s)))

// 成对探针：记录 setToolTip 的输入与最终传给 Qt 的输出，判定替换是否真的发生
static void pairlog(const void* in, const void* out) {
    if (!g_probe || !in) return;
    long long n = q_size(in);
    if (n < 40 || n > 4000) return;
    const u16* pi = q_utf16(in);
    if (!pi) return;
    std::wstring wi((const wchar_t*)pi, (size_t)n);
    std::wstring wo;
    long long m = out ? q_size(out) : 0;
    if (m > 0 && m < 8000) {
        const u16* po = q_utf16(out);
        if (po) wo.assign((const wchar_t*)po, (size_t)m);
    }
    {
        std::lock_guard<std::mutex> g(g_prmu);
        static std::unordered_set<std::wstring> seen;
        std::wstring key = std::to_wstring(g_tpath) + L"|" + wi;
        if (!seen.insert(key).second) return;
        if (seen.size() > 400) return;
    }
    bool same = (wo.empty() || wo == wi);
    bool sameptr = (out == in);
    std::wstring a = wi.size() > 1600 ? wi.substr(0, 1600) : wi;
    std::wstring b = wo.size() > 400 ? wo.substr(0, 400) : wo;
    std::wstring line = std::wstring(L"=== PAIR path=") + std::to_wstring(g_tpath) +
                        L" changed=" + (same ? L"0" : L"1") +
                        L" sameptr=" + (sameptr ? L"1" : L"0") +
                        L" in=" + std::to_wstring(n) + L" out=" + std::to_wstring(m) + L" ===\r\n" +
                        L"IN : " + a + L"\r\nOUT: " + b + L"\r\n";
    prwrite(line);
}

static void __cdecl h_tip_pair(VP a, const void* s) {
    g_hn = "w_setToolTip";
    const void* r = T(s);
    pairlog(s, r);
    o_w_setToolTip(a, r);
}
// PI 的悬浮提示主要走 QToolTip::showText（静态接口），不经过 setToolTip，
// 之前漏钩这个函数导致 tooltip 全部保持英文。
HOOK(void, w_tip_showText,   (const void* pos, const void* s, VP w, const void* rc, int ms),
                                                                      (pos, T(s), w, rc, ms))
// showText 的三参数重载（不带 rect / 时长）
HOOK(void, w_tip_showText3,  (const void* pos, const void* s, VP w),   (pos, T(s), w))
HOOK(void, w_setWhatsThis,   (VP a, const void* s),                    (a, T(s)))
HOOK(void, w_setStatusTip,   (VP a, const void* s),                    (a, T(s)))
HOOK(void, label_setText,    (VP a, const void* s),                    (a, T(s)))
HOOK(void, btn_setText,      (VP a, const void* s),                    (a, T(s)))
HOOK(void, action_setText,   (VP a, const void* s),                    (a, T(s)))
HOOK(void, menu_setTitle,    (VP a, const void* s),                    (a, T(s)))
HOOK(void, groupbox_setTitle,(VP a, const void* s),                    (a, T(s)))
HOOK(void, progress_setLabel,(VP a, const void* s),                    (a, T(s)))

HOOK(void*, w_addAction_t,   (VP a, const void* s),                    (a, T(s)))
HOOK(void*, menubar_addMenu_t,(VP a, const void* s),                   (a, T(s)))
HOOK(void*, menu_addMenu_t,  (VP a, const void* s),                    (a, T(s)))
HOOK(int,   tab_addTab_wt,   (VP a, VP w, const void* s),              (a, w, T(s)))

HOOK(void*, menu_addAction_it, (VP a, const void* ic, const void* s),  (a, ic, T(s)))
HOOK(void*, menu_addMenu_it,   (VP a, const void* ic, const void* s),  (a, ic, T(s)))
HOOK(void*, w_addAction_ik,    (VP a, const void* ic, const void* s, const void* k), (a, ic, T(s), k))
HOOK(void*, w_addAction_trm,   (VP a, const void* s, const void* r, const void* m),  (a, T(s), r, m))
HOOK(int,   tab_addTab_wit,    (VP a, VP w, const void* ic, const void* s),          (a, w, ic, T(s)))

HOOK(void*, w_addAction_itrm,  (VP a, const void* ic, const void* s, const void* r, const void* m), (a, ic, T(s), r, m))
HOOK(void*, w_addAction_trmc,  (VP a, const void* s, const void* r, const void* m, int ct),        (a, T(s), r, m, ct))

HOOK(void*, menu_addAction_6,  (VP a, const void* ic, const void* s, const void* r, const void* m, const void* k), (a, ic, T(s), r, m, k))
HOOK(void*, w_addAction_itrmc, (VP a, const void* ic, const void* s, const void* r, const void* m, int ct),        (a, ic, T(s), r, m, ct))

// 绘制层：文本在参数 3 / 4 / 2
// 注意：QRect 与 QRectF、QPoint 与 QPointF 的参数宽度不同（16 vs 32 / 8 vs 16 字节），
// 必须各自保留独立的原始函数指针，否则会把 QRectF 当 QRect 解引用，
// 得到非法的绘制矩形并触发访问冲突（0xc0000005）。
HOOK(void,  painter_rect,      (VP a, const void* rc, int f, const void* s, const void* br), (a, rc, f, T(s), br))
HOOK(void,  painter_rectf,     (VP a, const void* rc, int f, const void* s, const void* br), (a, rc, f, T(s), br))
HOOK(void,  painter_xy,        (VP a, int x, int y, const void* s),    (a, x, y, T(s)))
HOOK(void,  painter_ptf,       (VP a, const void* pt, const void* s),  (a, pt, T(s)))
HOOK(void,  painter_pt,        (VP a, const void* pt, const void* s),  (a, pt, T(s)))
HOOK(void,  painter_pt2,       (VP a, const void* pt, const void* s, int a1, int a2), (a, pt, T(s), a1, a2))
HOOK(void,  painter_rectopt,   (VP a, const void* rc, const void* s, const void* opt), (a, rc, T(s), opt))
HOOK(void,  painter_xywh,      (VP a, int x, int y, int w, int h, int f, const void* s, const void* br),
                               (a, x, y, w, h, f, T(s), br))
// 竖排/艺术字（PI 侧边工具栏）：文本先写进 QPainterPath 再 drawText，
// 或者缓存在 QStaticText 里（setText/构造函数入场）。
HOOK(void,  painter_path,      (VP a, const void* path, const void* s), (a, path, T(s)))
HOOK(void,  sttext_setText,    (VP a, const void* s),                  (a, T(s)))
HOOK(void*, sttext_ctor_q,     (VP a, const void* s),                  (a, T(s)))
HOOK(void*, sttext_ctor_qf,    (VP a, const void* s, const void* f),   (a, T(s), f))

// 树/列表/表格单元格：Qt6 的 QCommonStyle 用 QTextLayout 逐行绘制，
// 完全绕过 QPainter::drawText，所以必须在这里替换文本。
HOOK(void,  tlayout_setText,   (VP a, const void* s),                  (a, T(s)))
// 树/表/列表单元格在 Qt6Widgets 里直接用构造函数建 QTextLayout，
// 构造函数内部再调 setText（Qt6Gui 模块内直呼），IAT 钩不到，故连构造函数一起钩。
HOOK(void,  tl_ctor_q,         (VP a, const void* s),                  (a, T(s)))
HOOK(void,  tl_ctor_qfd,       (VP a, const void* s, const void* f, const void* d), (a, T(s), f, d))
// 富文本（工具提示、帮助文字）走 QTextDocument
HOOK(void,  tdoc_plain,        (VP a, const void* s),                  (a, T(s)))
HOOK(void,  tdoc_html,         (VP a, const void* s),                  (a, T(s)))
// PixInsight 的「鼠标悬浮提示」是**可选中文本**的富文本气泡，由 QTextEdit 一族的
// 富文本接口设置内容（setHtml / setText / setPlainText），完全不经过
// QWidget::setToolTip 与 QToolTip::showText，所以之前挂的 tooltip 钩子全部漏掉它，
// 悬浮提示一直保持英文。这里补齐这条真实路径。
HOOK(void,  textedit_setHtml,  (VP a, const void* s),                  (a, T(s)))
HOOK(void,  textedit_setText,  (VP a, const void* s),                  (a, T(s)))
HOOK(void,  textedit_setPlain, (VP a, const void* s),                  (a, T(s)))
// 进程控制台：启动日志是逐行追加的，绕过上面这些“整体设置”接口
HOOK(void,  console_append,    (VP a, const void* s),                  (a, T(s)))
HOOK(void,  console_insertp,   (VP a, const void* s),                  (a, T(s)))
HOOK(void,  textedit_append,   (VP a, const void* s),                  (a, T(s)))
HOOK(void,  cursor_insertText, (VP a, const void* s, const void* f),   (a, T(s), f))

// Qt 标准控件的文本经常在**构造时**就传入，例如 Required Flags 对话框里的
//   new QCheckBox("Gold right ascension", parent)
// 这种路径完全不经过 setText，IAT 钩不到，于是复选框/单选/分组框/标签
// 一直是英文（Gaia 的标志项就是这样漏掉的）。
// 这里只替换传入的 QString 参数、不动 this；构造函数内部会自行拷贝文本，
// 缓存对象在 qcache 里长期存活，因此安全。
HOOK(void*, cb_ctor_q, (void* self, const void* s, void* parent),                 (self, T(s), parent))
HOOK(void*, rb_ctor_q, (void* self, const void* s, void* parent),                 (self, T(s), parent))
HOOK(void*, gb_ctor_q, (void* self, const void* s, void* parent),                 (self, T(s), parent))
HOOK(void,  combo_addItem,    (void* self, const void* s, const void* v),         (self, T(s), v))
HOOK(void,  combo_insertItem, (void* self, int i, const void* s, const void* v),  (self, i, T(s), v))
HOOK(void,  combo_setItemText,(void* self, int i, const void* s),                 (self, i, T(s)))

// ---------- 下拉框批量插入 ----------
// PI 主程序大量使用 QComboBox::insertItems(index, QStringList) / addItems(QStringList)
// 一次性灌入整份国家名、语言名、像素运算符表。这条路径既不经过 addItem/insertItem，
// 也不经过 setText，所以之前那批短串（Sri Lanka / Hiri Motu / A over B …）永远是英文。
//
// 入参 list 是 const 引用，而且是隐式共享（COW）对象，同一个 QStringList 可能被多个
// 控件共用。直接改写会污染调用方内存、破坏引用计数，所以这里完全不碰 list：
// 先让原函数把英文插进去，再按插入前后的 count 差值，逐项 itemText 读回 → 翻译 → setItemText 写回。
// 任一符号解析失败就整体降级为“只调用原函数”，不会影响稳定性。
static int  (__cdecl* o_combo_count)(void*)                    = 0;
static void (__cdecl* o_combo_itemText)(void*, void*, int)     = 0;
static void (__cdecl* o_qstring_dtor)(void*)                   = 0;
static void (__cdecl* o_combo_insertItems)(void*, int, const void*) = 0;
static void (__cdecl* o_combo_addItems)(void*, const void*)    = 0;
static int g_batch_calls = 0;
static int g_batch_items = 0;
static int g_batch_ready = 0;

static void resolve_combo_batch() {
    if (g_batch_ready) return;
    HMODULE w = GetModuleHandleW(L"Qt6Widgets.dll");
    HMODULE c = GetModuleHandleW(L"Qt6Core.dll");
    if (!w || !c) return;
    o_combo_count    = (int  (__cdecl*)(void*))            GetProcAddress(w, "?count@QComboBox@@QEBAHXZ");
    o_combo_itemText = (void (__cdecl*)(void*, void*, int))GetProcAddress(w, "?itemText@QComboBox@@QEBA?AVQString@@H@Z");
    o_qstring_dtor   = (void (__cdecl*)(void*))            GetProcAddress(c, "??1QString@@QEAA@XZ");
    g_batch_ready = 1;
    logline("combo batch api: count=%d itemText=%d dtor=%d",
            (int)(o_combo_count != 0), (int)(o_combo_itemText != 0), (int)(o_qstring_dtor != 0));
}

static void retranslate_range(void* self, int from, int count) {
    for (int k = 0; k < count; ++k) {
        int idx = from + k;
        alignas(8) char qs[64];          // QString = {Data* d; char16_t* ptr; qsizetype size;}
        memset(qs, 0, sizeof(qs));
        o_combo_itemText(qs, self, idx);
        const void* tr = T((const void*)qs);
        if (tr && tr != (const void*)qs) o_combo_setItemText(self, idx, tr);
        o_qstring_dtor(qs);
    }
}

static void __cdecl h_combo_insertItems(void* self, int index, const void* list) {
    resolve_combo_batch();
    int before = o_combo_count ? o_combo_count(self) : -1;
    o_combo_insertItems(self, index, list);
    if (before < 0 || !o_combo_itemText || !o_qstring_dtor) return;
    int delta = o_combo_count(self) - before;
    if (delta <= 0 || delta > 20000) return;
    retranslate_range(self, index, delta);
    g_batch_items += delta;
    if (++g_batch_calls <= 4) logline("insertItems #%d idx=%d n=%d", g_batch_calls, index, delta);
}

static void __cdecl h_combo_addItems(void* self, const void* list) {
    resolve_combo_batch();
    int before = o_combo_count ? o_combo_count(self) : -1;
    o_combo_addItems(self, list);
    if (before < 0 || !o_combo_itemText || !o_qstring_dtor) return;
    int delta = o_combo_count(self) - before;
    if (delta <= 0 || delta > 20000) return;
    retranslate_range(self, before, delta);
    g_batch_items += delta;
    if (++g_batch_calls <= 4) logline("addItems #%d start=%d n=%d", g_batch_calls, before, delta);
}

HOOK(void*, lb_ctor_q, (void* self, const void* s, void* parent, unsigned int f), (self, T(s), parent, f))
// QPushButton 构造函数：文本在构造时传入，Qt6Widgets 内部直调 setText，
// 不经过模块 IAT 的 setText 导入，所以 FITS 头的 Add/Replace/Remove、
// 新建图像的 Set As Active Image、BlurXTerminator 的参数标签等一直漏翻。
HOOK(void*, pb_ctor_q,  (void* self, const void* s, void* parent),                (self, T(s), parent))
HOOK(void*, pb_ctor_iq, (void* self, const void* i, const void* s, void* parent), (self, i, T(s), parent))

// ---------- 目标表 ----------
struct HT { const char* name; void* hook; void** orig; };
static HT g_hooks[] = {
    { "?setWindowTitle@QWidget@@QEAAXAEBVQString@@@Z",       (void*)h_w_setWindowTitle, (void**)&o_w_setWindowTitle },
    { "?setToolTip@QWidget@@QEAAXAEBVQString@@@Z",           (void*)h_tip_pair,         (void**)&o_w_setToolTip },
    { "?showText@QToolTip@@SAXAEBVQPoint@@AEBVQString@@PEAVQWidget@@AEBVQRect@@H@Z",
                                                             (void*)h_w_tip_showText,   (void**)&o_w_tip_showText },
    { "?showText@QToolTip@@SAXAEBVQPoint@@AEBVQString@@PEAVQWidget@@@Z",
                                                             (void*)h_w_tip_showText3,  (void**)&o_w_tip_showText3 },
    { "?setWhatsThis@QWidget@@QEAAXAEBVQString@@@Z",         (void*)h_w_setWhatsThis,   (void**)&o_w_setWhatsThis },
    { "?setStatusTip@QWidget@@QEAAXAEBVQString@@@Z",         (void*)h_w_setStatusTip,   (void**)&o_w_setStatusTip },
    // 只保留“非结构性”文本设置接口。凡是会向菜单结构里插入字符串、
    // 或改变菜单/菜单栏标题本身（addAction、addMenu、setTitle@QMenu、addTab …）
    // 一律不挂钩：PixInsight 的菜单管理器会按英文标题回查已有菜单，标题被改成
    // 中文后回查失败就会不断追加新菜单，导致菜单栏出现成排重复项。
    // 但 QAction::setText 只是改写一个“已存在 action”的显示文本，不增删任何
    // 菜单项、也不参与菜单管理器回查，是安全的——右键菜单(context menu)里的
    // Undo/Redo 等就是靠它填充的，不挂则右键菜单永远英文。故单独补挂。

    // 控件本体文本（QLabel / 按钮 / 分组框 / 进度条）：与菜单结构无关，
    // 挂钩后参数对话框里的标签才能可靠替换（部分控件不走 drawText）。
    { "?setText@QLabel@@QEAAXAEBVQString@@@Z",               (void*)h_label_setText,    (void**)&o_label_setText },
    { "?setText@QAbstractButton@@QEAAXAEBVQString@@@Z",      (void*)h_btn_setText,      (void**)&o_btn_setText },
    // QAction::setText 实测（2026-11-22）仍然导致菜单栏成排重复：PI 的菜单管理器
    // 会反复用英文标题调 setText 并按 text() 回查，文本被替换成中文后回查失败，
    // 每轮都 addAction 新菜单。该通道彻底放弃，右键菜单英文只能靠词典整句 +
    // 绘制层覆盖，不能走 setText 替换。
    // 下拉框列表项：PI 用 addItem(text, data) 填选项，"Off"、"Preserve"、
    // "Mode1" 这类枚举值不经过任何 setText，只能在入场处替换。
    { "?addItem@QComboBox@@QEAAXAEBVQString@@AEBVQVariant@@@Z",
                                                             (void*)h_combo_addItem,    (void**)&o_combo_addItem },
    { "?insertItem@QComboBox@@QEAAXHAEBVQString@@AEBVQVariant@@@Z",
                                                             (void*)h_combo_insertItem, (void**)&o_combo_insertItem },
    { "?setItemText@QComboBox@@QEAAXHAEBVQString@@@Z",       (void*)h_combo_setItemText,(void**)&o_combo_setItemText },
    // 批量插入（QStringList 版本）：国家名/语言名/运算符表走这条路径
    { "?insertItems@QComboBox@@QEAAXHAEBVQStringList@@@Z",   (void*)h_combo_insertItems,(void**)&o_combo_insertItems },
    { "?addItems@QComboBox@@QEAAXAEBVQStringList@@@Z",       (void*)h_combo_addItems,   (void**)&o_combo_addItems },
    // 带文本的构造函数：复选框/单选按钮/分组框/标签的初始文本走这里，setText 钩不到
    { "??0QCheckBox@@QEAA@AEBVQString@@PEAVQWidget@@@Z",     (void*)h_cb_ctor_q,        (void**)&o_cb_ctor_q },
    { "??0QRadioButton@@QEAA@AEBVQString@@PEAVQWidget@@@Z",  (void*)h_rb_ctor_q,        (void**)&o_rb_ctor_q },
    { "??0QGroupBox@@QEAA@AEBVQString@@PEAVQWidget@@@Z",     (void*)h_gb_ctor_q,        (void**)&o_gb_ctor_q },
    { "??0QLabel@@QEAA@AEBVQString@@PEAVQWidget@@V?$QFlags@W4WindowType@Qt@@@@@Z",
                                                             (void*)h_lb_ctor_q,        (void**)&o_lb_ctor_q },
    { "??0QPushButton@@QEAA@AEBVQString@@PEAVQWidget@@@Z",   (void*)h_pb_ctor_q,        (void**)&o_pb_ctor_q },
    { "??0QPushButton@@QEAA@AEBVQIcon@@AEBVQString@@PEAVQWidget@@@Z",
                                                             (void*)h_pb_ctor_iq,       (void**)&o_pb_ctor_iq },
    { "?setTitle@QGroupBox@@QEAAXAEBVQString@@@Z",           (void*)h_groupbox_setTitle,(void**)&o_groupbox_setTitle },
    { "?setLabelText@QProgressDialog@@QEAAXAEBVQString@@@Z", (void*)h_progress_setLabel,(void**)&o_progress_setLabel },

    { "?drawText@QPainter@@QEAAXAEBVQRect@@HAEBVQString@@PEAV2@@Z",  (void*)h_painter_rect,  (void**)&o_painter_rect },
    { "?drawText@QPainter@@QEAAXAEBVQRectF@@HAEBVQString@@PEAV2@@Z", (void*)h_painter_rectf, (void**)&o_painter_rectf },
    { "?drawText@QPainter@@QEAAXHHAEBVQString@@@Z",                  (void*)h_painter_xy,    (void**)&o_painter_xy },
    { "?drawText@QPainter@@QEAAXAEBVQPointF@@AEBVQString@@@Z",       (void*)h_painter_ptf,   (void**)&o_painter_ptf },
    { "?drawText@QPainter@@QEAAXAEBVQPoint@@AEBVQString@@@Z",        (void*)h_painter_pt,    (void**)&o_painter_pt },
    { "?drawText@QPainter@@QEAAXAEBVQPointF@@AEBVQString@@HH@Z",     (void*)h_painter_pt2,   (void**)&o_painter_pt2 },
    { "?drawText@QPainter@@QEAAXAEBVQRectF@@AEBVQString@@AEBVQTextOption@@@Z", (void*)h_painter_rectopt, (void**)&o_painter_rectopt },
    { "?drawText@QPainter@@QEAAXHHHHHAEBVQString@@PEAVQRect@@@Z",    (void*)h_painter_xywh, (void**)&o_painter_xywh },
    // 竖排/艺术字：QPainterPath 版 drawText 与 QStaticText 缓存通道
    { "?drawText@QPainter@@QEAAXAEBVQPainterPath@@AEBVQString@@@Z",  (void*)h_painter_path,     (void**)&o_painter_path },
    { "?setText@QStaticText@@QEAAXAEBVQString@@@Z",                  (void*)h_sttext_setText,   (void**)&o_sttext_setText },
    { "??0QStaticText@@QEAA@AEBVQString@@@Z",                        (void*)h_sttext_ctor_q,    (void**)&o_sttext_ctor_q },
    { "??0QStaticText@@QEAA@AEBVQString@@AEBVQFont@@@Z",             (void*)h_sttext_ctor_qf,   (void**)&o_sttext_ctor_qf },

    { "?setText@QTextLayout@@QEAAXAEBVQString@@@Z",                  (void*)h_tlayout_setText, (void**)&o_tlayout_setText },
    { "??0QTextLayout@@QEAA@AEBVQString@@@Z",                        (void*)h_tl_ctor_q,   (void**)&o_tl_ctor_q },
    { "??0QTextLayout@@QEAA@AEBVQString@@AEBVQFont@@PEBVQPaintDevice@@@Z", (void*)h_tl_ctor_qfd, (void**)&o_tl_ctor_qfd },
    { "?setPlainText@QTextDocument@@QEAAXAEBVQString@@@Z",           (void*)h_tdoc_plain,   (void**)&o_tdoc_plain },
    { "?setHtml@QTextDocument@@QEAAXAEBVQString@@@Z",                (void*)h_tdoc_html,    (void**)&o_tdoc_html },
    { "?setHtml@QTextEdit@@QEAAXAEBVQString@@@Z",                    (void*)h_textedit_setHtml,  (void**)&o_textedit_setHtml },
    { "?setText@QTextEdit@@QEAAXAEBVQString@@@Z",                    (void*)h_textedit_setText,  (void**)&o_textedit_setText },
    { "?setPlainText@QTextEdit@@QEAAXAEBVQString@@@Z",               (void*)h_textedit_setPlain, (void**)&o_textedit_setPlain },

    { "?appendPlainText@QPlainTextEdit@@QEAAXAEBVQString@@@Z",       (void*)h_console_append,  (void**)&o_console_append },
    { "?insertPlainText@QPlainTextEdit@@QEAAXAEBVQString@@@Z",       (void*)h_console_insertp, (void**)&o_console_insertp },
    { "?append@QTextEdit@@QEAAXAEBVQString@@@Z",                     (void*)h_textedit_append, (void**)&o_textedit_append },
    { "?insertText@QTextCursor@@QEAAXAEBVQString@@AEBVQTextCharFormat@@@Z", (void*)h_cursor_insertText, (void**)&o_cursor_insertText },
};
static const int NHOOK = (int)(sizeof(g_hooks) / sizeof(g_hooks[0]));

static int find_hook(const char* name) {
    for (int i = 0; i < NHOOK; ++i) if (strcmp(g_hooks[i].name, name) == 0) return i;
    return -1;
}

static int g_patched = 0;

// PI 会在启动过程中动态加载/卸载插件模块，EnumProcessModules 拿到的句柄
// 可能已被卸载，直接解引用 PE 头会访问冲突。这里逐步校验内存可读性。
static bool mem_ok(const void* p, size_t n) {
    if (!p || !n) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    BYTE* s = (BYTE*)p;
    BYTE* e = s + n;
    BYTE* bs = (BYTE*)mbi.BaseAddress;
    BYTE* be = bs + mbi.RegionSize;
    return s >= bs && e <= be && e >= s;
}

static int safe_find_hook(const char* name) {
    for (int k = 0; k < 256; ++k) {
        if (!mem_ok(name + k, 1)) return -1;
        if (!name[k]) return find_hook(name);
    }
    return -1;
}

static void patch_module_inner(HMODULE m) {
    if (!m) return;
    BYTE* base = (BYTE*)m;
    if (!mem_ok(base, sizeof(IMAGE_DOS_HEADER))) return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return;
    if (!mem_ok(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) return;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
    DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    DWORD impSz = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    if (!impRva || impSz < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return;
    if (!mem_ok(base + impRva, sizeof(IMAGE_IMPORT_DESCRIPTOR))) return;
    IMAGE_IMPORT_DESCRIPTOR* id = (IMAGE_IMPORT_DESCRIPTOR*)(base + impRva);
    int guard = 0;
    for (; guard < 512; ++id, ++guard) {
        if (!mem_ok(id, sizeof(*id))) return;
        if (!id->Name) break;
        if (!id->OriginalFirstThunk || !id->FirstThunk) continue;
        if (!mem_ok(base + id->OriginalFirstThunk, sizeof(IMAGE_THUNK_DATA))) continue;
        if (!mem_ok(base + id->FirstThunk, sizeof(IMAGE_THUNK_DATA))) continue;
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(base + id->OriginalFirstThunk);
        IMAGE_THUNK_DATA* ft  = (IMAGE_THUNK_DATA*)(base + id->FirstThunk);
        for (int i = 0; i < 65536; ++i) {
            if (!mem_ok(&oft[i], sizeof(IMAGE_THUNK_DATA))) break;
            if (!oft[i].u1.AddressOfData) break;
            if (!mem_ok(&ft[i], sizeof(IMAGE_THUNK_DATA))) break;
            if (oft[i].u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            ULONGLONG ad = (ULONGLONG)oft[i].u1.AddressOfData;
            if (ad >= (ULONGLONG)nt->OptionalHeader.SizeOfImage) continue;
            if (!mem_ok(base + (DWORD)ad, sizeof(IMAGE_IMPORT_BY_NAME))) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + (DWORD)ad);
            int hi = safe_find_hook((const char*)ibn->Name);
            if (hi < 0) continue;
            HT& h = g_hooks[hi];
            if ((void*)ft[i].u1.Function == h.hook) continue;
            DWORD old = 0;
            if (VirtualProtect(&ft[i].u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                if (*h.orig == 0) *h.orig = (void*)ft[i].u1.Function;
                ft[i].u1.Function = (ULONG_PTR)h.hook;
                VirtualProtect(&ft[i].u1.Function, sizeof(void*), old, &old);
                ++g_patched;
            }
        }
    }
}

static void patch_module(HMODULE m) {
    __try { patch_module_inner(m); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static void allow_cfg(void* addr) {
    typedef BOOL (WINAPI* pSVCT)(HANDLE, void*, SIZE_T, ULONG, void*);
    HMODULE k = GetModuleHandleW(L"kernelbase.dll");
    if (!k) return;
    pSVCT f = (pSVCT)GetProcAddress(k, "SetProcessValidCallTargets");
    if (!f) return;
    struct { ULONG_PTR Offset; ULONG Flags; } ci;
    ULONG_PTR page = (ULONG_PTR)addr & ~(ULONG_PTR)0xFFF;
    ci.Offset = (ULONG_PTR)addr - page;
    ci.Flags = 1;
    f(GetCurrentProcess(), (void*)page, 0x1000, 1, &ci);
}

// 自检 3：长富文本词条走完整 T() 链路。
// 词条明明在词典里、pi_miss 也为空，界面却仍是英文 —— 只有两种可能：
//   a) 命中后译文对象构建失败，T() 退回原文（路径 g_tpath=6）
//   b) 查找没问题，但显示那一步根本没经过我们钩住的函数
// 这一步在进程内自跑，不需要界面操作就能把 a / b 区分开。
static void selftest_rich() {
    if (g_keys.empty()) return;
    std::unordered_set<std::wstring>* svs = g_seen;
    std::unordered_set<std::wstring>* svm = g_miss;
    g_seen = 0; g_miss = 0;              // 自检文本不污染采集日志
    int probed = 0, ok = 0, buildfail = 0, reported = 0;
    for (size_t i = 0; i < g_keys.size() && probed < 40; ++i) {
        const std::wstring& k = g_keys[i];
        if (k.size() < 200 || k.size() > 900) continue;
        if (k.find(L"<p>") == std::wstring::npos) continue;
        ++probed;
        std::string u8 = w2u8(k);
        void* qs = calloc(1, 64);
        q_fromUtf8(qs, u8.c_str(), (long long)u8.size());
        if (q_size(qs) <= 0) { free(qs); ++buildfail; continue; }
        const void* r = T(qs);
        bool good = false;
        if (r && r != (const void*)qs) {
            long long rn = q_size(r);
            const u16* rp = rn > 0 ? q_utf16(r) : 0;
            if (rp) {
                for (long long j = 0; j < rn; ++j) {
                    if (rp[j] >= 0x4E00 && rp[j] <= 0x9FFF) { good = true; break; }
                }
            }
        }
        if (good) ++ok;
        else if (reported < 3) {
            ++reported;
            logline("selftest rich-tip bad: len=%d tpath=%d", (int)k.size(), g_tpath);
        }
        free(qs);
    }
    g_seen = svs; g_miss = svm;
    logline("selftest rich-tip: probed=%d ok=%d buildfail=%d", probed, ok, buildfail);
}

static DWORD WINAPI worker(LPVOID) {
    logline("worker enter");
    HMODULE core = 0;
    for (int i = 0; i < 600 && !core; ++i) { core = GetModuleHandleW(L"Qt6Core.dll"); if (!core) Sleep(50); }
    if (!core) { logline("Qt6Core.dll not found"); return 0; }
    q_fromUtf8 = (pfn_fromUtf8)GetProcAddress(core, "?fromUtf8@QString@@SA?AV1@PEBD_J@Z");
    q_size     = (pfn_qsize)GetProcAddress(core, "?size@QString@@QEBA_JXZ");
    q_utf16    = (pfn_utf16)GetProcAddress(core, "?utf16@QString@@QEBAPEBGXZ");
    logline("api resolved %d %d %d", (int)(q_fromUtf8 != 0), (int)(q_size != 0), (int)(q_utf16 != 0));
    if (!q_fromUtf8 || !q_size || !q_utf16) { logline("QString api missing"); return 0; }

    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_qcs);
    g_qcs_ok = true;
    builddict();
    char rec[8] = {0};
    if (GetEnvironmentVariableA("PI_ZH_RECORD", rec, sizeof(rec)) && rec[0] == '1') {
        g_seen = new std::unordered_set<std::wstring>();
        logline("record mode on");
    }
    if (GetEnvironmentVariableA("PI_ZH_MISS", rec, sizeof(rec)) && rec[0] == '1') {
        g_miss = new std::unordered_set<std::wstring>();
        logline("miss log on");
    }
    if (GetEnvironmentVariableA("PI_ZH_DIAG", rec, sizeof(rec)) && rec[0] == '1') {
        g_diag = 1;
        logline("diag on");
    }
    if (GetEnvironmentVariableA("PI_ZH_PROBE", rec, sizeof(rec)) && rec[0] == '1') {
        g_probe = 1;
        logline("probe on");
    }
    if (g_keys.empty() && !g_seen) { logline("empty dict, skip hooks"); return 0; }
    InterlockedExchange(&g_ready, 1);
    selftest_rich();
    for (int i = 0; i < NHOOK; ++i) allow_cfg(g_hooks[i].hook);

    // 早期版本只跑 60 轮（约 50 秒）就退出，之后才被加载的插件模块（例如迟开的
    // Gaia 星表进程界面所在的 Gaia-pxm.dll）其 IAT 永远不会被挂钩，界面里那些
    // tooltip 就一直是英文。改为长时间驻留，间隔放缓，开销可以忽略。
    for (long long round = 0; ; ++round) {
        HMODULE mods[768]; DWORD need = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
            int n = (int)(need / sizeof(HMODULE));
            if (n > 768) n = 768;
            for (int i = 0; i < n; ++i) patch_module(mods[i]);
        }
        if (round == 0) logline("round0 patched %d", g_patched);
        Sleep(round < 12 ? 150 : 500);
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        logline("dll attach");
        HANDLE t = CreateThread(0, 0, worker, 0, 0, 0);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
