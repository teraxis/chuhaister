// cswap-tray — native Win32 system tray for claude-swap.
//
// A thin shell over the claude-swap CLI: all account/usage/switch logic stays
// in the Python package; this exe only renders. Data comes from
// `cswap list --json` (fetched on a background thread), switching runs
// `cswap switch <num>`. No frameworks — idle cost is a message loop and one
// hidden window.
//
//   * left click           -> toggle a slide-out panel: one card per account
//     with 5h / weekly / per-model limits, progress bars, reset countdowns,
//     and an "Active agent" toggle that switches accounts in place. Dismissed
//     by clicking the icon again or anywhere outside (a WH_MOUSE_LL hook that
//     lives only while the panel is open). Rendered with GDI+ (anti-aliased).
//   * right click          -> menu: accounts, switch strategies, refresh,
//     "Start with Windows" (HKCU Run), open log, quit
//   * the icon itself      -> colored disc (green/amber/red) + utilization %
//
// Build (Zig as the C++ toolchain, static, no console) — see build.ps1:
//   zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode cswap_tray.cpp \
//       -o cswap-tray.exe -lgdi32 -lshell32 -ladvapi32 -lgdiplus -lole32 \
//       -static -Wl,--subsystem,windows
//
// cswap.exe discovery order: CSWAP_TRAY_CSWAP env var; "cswap" field of
// %USERPROFILE%\.claude-swap-backup\tray_native.json; next to this exe;
// ..\.venv\Scripts\cswap.exe relative to this exe (repo layout); PATH.

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10+: DPI-awareness-context APIs
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>

#include "core/oauth_login.h"   // browser login (also pulls vault/oauth/http)
#include "core/switcher.h"      // live Claude Code state + the switch itself

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace Gdiplus;

// ---------------------------------------------------------------- constants

static const wchar_t* APP_NAME = L"chuhAIster";
static const wchar_t* APP_VERSION = L"0.1.0";
static const wchar_t* APP_URL = L"https://github.com/teraxis/chuhaister";
static const wchar_t* MUTEX_NAME = L"Local\\chuhaister-single-instance";
static const wchar_t* AUTOSTART_VALUE = L"chuhAIster";

static const UINT WM_TRAYCB = WM_APP + 1;   // tray icon callback
static const UINT WM_MODEL = WM_APP + 2;    // wParam: Model* (heap, ownership passes)
static const UINT WM_CSWAP_ERR = WM_APP + 3; // wParam: std::wstring* (heap)
static const UINT WM_HIDEPANEL = WM_APP + 4; // outside click detected by the mouse hook

static const int DEFAULT_REFRESH_S = 60;

// palette (matches the Python tray / Claude Code popover)
static const COLORREF C_BG = RGB(0x16, 0x16, 0x16);
static const COLORREF C_CARD = RGB(0x1e, 0x1e, 0x1e);
static const COLORREF C_BORDER = RGB(0x2c, 0x2c, 0x2c);
static const COLORREF C_GREEN = RGB(0x3f, 0xb9, 0x50);
static const COLORREF C_GREEN_DIM = RGB(0x2e, 0xa0, 0x43);
static const COLORREF C_BLUE = RGB(0x31, 0x6d, 0xca);
static const COLORREF C_TEXT = RGB(0xe6, 0xe6, 0xe6);
static const COLORREF C_MUTED = RGB(0x9d, 0x9d, 0x9d);
static const COLORREF C_BAR_BG = RGB(0x33, 0x33, 0x33);
static const COLORREF C_AMBER = RGB(0xdb, 0x9a, 0x04);
static const COLORREF C_RED = RGB(0xda, 0x36, 0x33);
static const COLORREF C_GRAY = RGB(0x80, 0x80, 0x80);
static const COLORREF C_KNOB_OFF = RGB(0x4a, 0x4a, 0x4a);

// ---------------------------------------------------------------- utf-8

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

// ---------------------------------------------------------------- tiny JSON
// Minimal recursive-descent parser for the fixed `cswap --json` shape; keeps
// the exe dependency-free. Numbers become double, strings stay UTF-8.

struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const char* key) const {
        if (type != OBJ) return nullptr;
        for (auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double numOr(double def) const { return type == NUM ? num : def; }
    std::string strOr(const char* def) const { return type == STR ? str : def; }
    bool boolOr(bool def) const { return type == BOOL ? b : def; }
};

struct JParser {
    const char* p;
    const char* end;
    bool ok = true;

    explicit JParser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool eat(char c) { ws(); if (p < end && *p == c) { ++p; return true; } return false; }

    JVal parse() { JVal v = value(); ws(); return v; }

    JVal value() {
        ws();
        if (p >= end) { ok = false; return {}; }
        switch (*p) {
            case '{': return object();
            case '[': return array();
            case '"': { JVal v; v.type = JVal::STR; v.str = string(); return v; }
            case 't': p += 4; { JVal v; v.type = JVal::BOOL; v.b = true; return v; }
            case 'f': p += 5; { JVal v; v.type = JVal::BOOL; v.b = false; return v; }
            case 'n': p += 4; return {};
            default: return number();
        }
    }

    JVal object() {
        JVal v; v.type = JVal::OBJ; ++p; // '{'
        ws();
        if (eat('}')) return v;
        while (ok && p < end) {
            ws();
            if (*p != '"') { ok = false; break; }
            std::string key = string();
            if (!eat(':')) { ok = false; break; }
            v.obj.emplace_back(std::move(key), value());
            if (eat(',')) continue;
            eat('}');
            break;
        }
        return v;
    }

    JVal array() {
        JVal v; v.type = JVal::ARR; ++p; // '['
        ws();
        if (eat(']')) return v;
        while (ok && p < end) {
            v.arr.push_back(value());
            if (eat(',')) continue;
            eat(']');
            break;
        }
        return v;
    }

    std::string string() {
        std::string out; ++p; // '"'
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (p + 4 < end) {
                            unsigned cp = 0;
                            sscanf(p + 1, "%4x", &cp);
                            p += 4;
                            // encode BMP code point as UTF-8 (surrogates are
                            // rare in this data; unpaired ones pass through)
                            if (cp < 0x80) out += (char)cp;
                            else if (cp < 0x800) {
                                out += (char)(0xC0 | (cp >> 6));
                                out += (char)(0x80 | (cp & 0x3F));
                            } else {
                                out += (char)(0xE0 | (cp >> 12));
                                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                out += (char)(0x80 | (cp & 0x3F));
                            }
                        }
                        break;
                    }
                    default: out += *p; break;
                }
            } else {
                out += *p;
            }
            ++p;
        }
        if (p < end) ++p; // closing '"'
        return out;
    }

    JVal number() {
        JVal v; v.type = JVal::NUM;
        char* np = nullptr;
        v.num = strtod(p, &np);
        if (np == p) { ok = false; return {}; }
        p = np;
        return v;
    }
};

// ---------------------------------------------------------------- model

struct LimitRow {
    std::wstring label;
    double pct = 0;
    std::wstring resets; // live countdown, empty when unknown/past
};

struct Acct {
    int num = 0;
    bool active = false;
    bool disabled = false;
    std::wstring title;  // "Claude (alias)" or email local part
    std::wstring email;
    std::wstring note;   // sentinel usageStatus when != ok
    std::vector<LimitRow> rows;
};

struct Model {
    std::vector<Acct> accounts;
};

// "2026-07-17T12:00:00.124819+00:00" -> unix seconds; 0 on failure.
static time_t parseIso8601(const std::string& s) {
    int y, mo, d, h, mi, se;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    struct tm tmv = {};
    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
    tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = se;
    time_t t = _mkgmtime(&tmv);
    if (t == (time_t)-1) return 0;
    // apply a +hh:mm / -hh:mm suffix (Z or missing = UTC)
    size_t tail = s.find_first_of("+-", 19);
    if (tail != std::string::npos) {
        int oh = 0, om = 0;
        if (sscanf(s.c_str() + tail + 1, "%d:%d", &oh, &om) >= 1)
            t -= (s[tail] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    }
    return t;
}

static std::wstring countdownFrom(time_t resetsAt) {
    if (!resetsAt) return L"";
    long long rem = (long long)resetsAt - (long long)time(nullptr);
    if (rem <= 0) return L"";
    long long days = rem / 86400, hrs = (rem % 86400) / 3600, mins = (rem % 3600) / 60;
    wchar_t buf[32];
    if (days > 0) swprintf(buf, 32, L"%lldd %lldh", days, hrs);
    else if (hrs > 0) swprintf(buf, 32, L"%lldh %lldm", hrs, mins);
    else swprintf(buf, 32, L"%lldm", mins);
    return buf;
}

// One usage window ({pct, resetsAt, countdown}) -> a panel row.
static bool windowRow(const JVal* w, const std::wstring& label, LimitRow& out) {
    if (!w || w->type != JVal::OBJ) return false;
    const JVal* pct = w->get("pct");
    if (!pct || pct->type != JVal::NUM) return false;
    out.label = label;
    out.pct = pct->num;
    out.resets = countdownFrom(parseIso8601(w->get("resetsAt") ? w->get("resetsAt")->strOr("") : ""));
    if (out.resets.empty() && w->get("countdown"))
        out.resets = widen(w->get("countdown")->strOr(""));  // frozen fallback
    return true;
}

static std::wstring localPart(const std::wstring& email, size_t limit = 24) {
    size_t at = email.find(L'@');
    std::wstring local = at == std::wstring::npos ? email : email.substr(0, at);
    if (local.size() > limit) local = local.substr(0, limit - 1) + L"*";
    return local;
}

// ---- native model building (the core replaces the old Python CLI) -----------

// Usage is rate-limited per token, so it is cached per account and refetched
// on a cadence: the active account often (it is what the icon shows), the
// others rarely. A failed fetch keeps serving the last good value.
struct UsageCache {
    cswap::Usage usage;
    time_t fetchedAt = 0;
    bool valid = false;
};
static std::map<int, UsageCache> g_usageCache;

static const int USAGE_TTL_ACTIVE = 60;   // seconds
static const int USAGE_TTL_IDLE = 300;

static bool accountUsage(cswap::Vault& v, const cswap::Account& a, bool active,
                         const cswap::LiveState& live, cswap::Usage& out) {
    UsageCache& c = g_usageCache[a.num];
    time_t now = time(nullptr);
    if (c.valid && now - c.fetchedAt < (active ? USAGE_TTL_ACTIVE : USAGE_TTL_IDLE)) {
        out = c.usage;
        return true;
    }
    auto serveStale = [&]() { if (c.valid) { out = c.usage; return true; } return false; };

    // For the active account the live file is the source of truth — Claude Code
    // keeps its token fresh and owns its rotation. Reading the vault copy and
    // refreshing it ourselves would rotate a shared refresh token twice and
    // invalidate the live login. Inactive accounts are ours to refresh.
    std::string credJson;
    bool fromLive = active && live.haveCred && live.haveIdentity && live.email == a.email;
    if (fromLive) credJson = live.credJson;
    else if (!v.readCred(a.num, credJson)) return serveStale();

    cswap::OAuthCred cred = cswap::parseCredentials(credJson);
    if (!cred.valid) return serveStale();
    if (cswap::tokenExpired(cred)) {
        if (fromLive) return serveStale();  // Claude Code will refresh the live token
        if (cswap::refreshCredentials(cred) != cswap::RefreshError::None) return serveStale();
        v.writeCred(a.num, cswap::serializeCredentials(cred));
    }
    cswap::Usage u = cswap::fetchUsage(cred.accessToken);
    if (!u.ok) return serveStale();
    c.usage = u;
    c.fetchedAt = now;
    c.valid = true;
    out = u;
    return true;
}

static void addRow(Acct& acc, const std::wstring& label, double pct, const std::string& resetsAt) {
    LimitRow r;
    r.label = label;
    r.pct = pct;
    r.resets = countdownFrom(parseIso8601(resetsAt));
    acc.rows.push_back(r);
}

static Model buildModel() {
    Model m;
    cswap::Vault v;
    if (!v.load()) return m;
    cswap::LiveState live = cswap::readLiveState();
    // Learn the active account's full profile while it is live, so a later
    // switch back to it restores Claude Code's own fields rather than a
    // synthesized stub. No-op once stored and unchanged.
    cswap::captureLiveProfile(v, live);
    // Keep the active account's stored credential in step with Claude Code's
    // live refresh-token rotation, so it doesn't die after a reboot without a
    // switch (see captureLiveCredential).
    cswap::captureLiveCredential(v, live);
    for (const cswap::Account& a : v.accounts) {
        Acct acc;
        acc.num = a.num;
        acc.email = widen(a.email);
        acc.title = localPart(acc.email);
        acc.active = live.haveIdentity && live.email == a.email;
        acc.disabled = a.disabled;
        cswap::Usage u;
        if (accountUsage(v, a, acc.active, live, u)) {
            if (u.fiveHour.present) addRow(acc, L"5-hour limit", u.fiveHour.pct, u.fiveHour.resetsAt);
            if (u.sevenDay.present) addRow(acc, L"Weekly · all models", u.sevenDay.pct, u.sevenDay.resetsAt);
            for (const cswap::ScopedWindow& s : u.scoped)
                addRow(acc, L"Weekly · " + widen(s.name), s.pct, s.resetsAt);
        }
        if (acc.rows.empty()) acc.note = L"usage unavailable";
        m.accounts.push_back(std::move(acc));
    }
    return m;
}

// binding (max) utilization of the active account; -1 when unknown
static double activePct(const Model& m) {
    for (const Acct& a : m.accounts)
        if (a.active) {
            double best = -1;
            for (const LimitRow& r : a.rows) best = r.pct > best ? r.pct : best;
            return best;
        }
    return -1;
}

// ---------------------------------------------------------------- local paths

static bool fileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring exeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path = buf;
    size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

static std::wstring backupDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    std::wstring home = n ? buf : L".";
    return home + L"\\.claude-swap-backup";
}

static std::string readFileUtf8(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string out;
    char buf[4096];
    DWORD got;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(f);
    return out;
}

// ---------------------------------------------------------------- globals

static HWND g_hwnd = nullptr;        // hidden message window
static HWND g_panel = nullptr;       // hover panel
static NOTIFYICONDATAW g_nid = {};
static Model g_model;
static bool g_haveModel = false;
static HANDLE g_refreshNow = nullptr; // event: fetch immediately
static volatile bool g_running = true;
static int g_refreshSec = DEFAULT_REFRESH_S;
static UINT g_taskbarCreatedMsg = 0;
static double g_scale = 1.0;         // DPI scale (system)
static HHOOK g_mouseHook = nullptr;  // installed only while the panel is open
static DWORD g_panelHiddenTick = 0;  // when the panel was last dismissed (icon-click debounce)
static ULONG_PTR g_gdipToken = 0;

static int S(int dip) { return (int)(dip * g_scale + 0.5); }

// ---------------------------------------------------------------- GDI+ helpers

// COLORREF (0x00BBGGRR) -> Gdiplus::Color, with optional alpha.
static Color gpc(COLORREF c, BYTE a = 255) {
    return Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

// Rounded-rectangle figure in device pixels.
static void addRoundRect(GraphicsPath& path, REAL x, REAL y, REAL w, REAL h, REAL r) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    REAL d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

static void fillRoundRectGp(Graphics& g, REAL x, REAL y, REAL w, REAL h, REAL r,
                            Color fill, bool stroke = false, Color border = Color()) {
    GraphicsPath path;
    addRoundRect(path, x, y, w, h, r);
    SolidBrush b(fill);
    g.FillPath(&b, &path);
    if (stroke) {
        Pen p(border, 1.0f);
        g.DrawPath(&p, &path);
    }
}

enum GAlign { GA_LEFT, GA_RIGHT, GA_CENTER };

// Draw a single text run. GenericTypographic removes the ~1/6-em padding GDI+
// normally adds, so x/y line up with the layout math; for right/center the
// anchor is the string's right edge / midpoint.
static void gtext(Graphics& g, const Font& font, const std::wstring& s, REAL x, REAL y,
                  Color color, GAlign align = GA_LEFT) {
    if (s.empty()) return;
    SolidBrush brush(color);
    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoClip | StringFormatFlagsNoWrap);
    if (align != GA_LEFT) {
        RectF bound;
        g.MeasureString(s.c_str(), -1, &font, PointF(0, 0), &fmt, &bound);
        x -= align == GA_RIGHT ? bound.Width : bound.Width / 2;
    }
    g.DrawString(s.c_str(), -1, &font, PointF(x, y), &fmt, &brush);
}

static REAL gmeasure(Graphics& g, const Font& font, const std::wstring& s) {
    StringFormat fmt(StringFormat::GenericTypographic());
    RectF bound;
    g.MeasureString(s.c_str(), -1, &font, PointF(0, 0), &fmt, &bound);
    return bound.Width;
}

// ---------------------------------------------------------------- tray icon

static HICON makeIcon(double pct) {
    const int sz = GetSystemMetrics(SM_CXSMICON) * 2; // draw 2x, shell downscales crisply
    COLORREF fill = pct < 0 ? C_GRAY : pct >= 95 ? C_RED : pct >= 80 ? C_AMBER : C_GREEN_DIM;

    Bitmap bmp(sz, sz, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    SolidBrush disc(gpc(fill));
    g.FillEllipse(&disc, 1.0f, 1.0f, (REAL)(sz - 2), (REAL)(sz - 2));

    wchar_t label[8];
    if (pct < 0) wcscpy(label, L"⇄");   // ⇄
    else if (pct >= 100) wcscpy(label, L"!");
    else swprintf(label, 8, L"%.0f", pct);

    FontFamily ff(L"Segoe UI");
    REAL em = (REAL)sz * (wcslen(label) >= 2 ? 0.46f : 0.60f);
    Font font(&ff, em, FontStyleBold, UnitPixel);
    SolidBrush white(Color(255, 255, 255, 255));
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    RectF box(0, 0, (REAL)sz, (REAL)sz);
    g.DrawString(label, -1, &font, box, &sf, &white);

    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

static void updateTray() {
    double pct = g_haveModel ? activePct(g_model) : -1;
    HICON icon = makeIcon(pct);
    HICON old = g_nid.hIcon;
    g_nid.hIcon = icon;

    std::wstring tip = APP_NAME;
    for (const Acct& a : g_model.accounts)
        if (a.active) {
            tip = a.title;
            for (const LimitRow& r : a.rows) {
                tip += L"\n" + r.label + L" " + std::to_wstring((int)r.pct) + L"%";
                if (tip.size() > 110) break;
            }
            break;
        }
    wcsncpy(g_nid.szTip, tip.c_str(), 127);
    g_nid.szTip[127] = 0;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (old) DestroyIcon(old);
}

static void notify(const std::wstring& title, const std::wstring& text) {
    g_nid.uFlags = NIF_INFO;
    wcsncpy(g_nid.szInfoTitle, title.c_str(), 63);
    g_nid.szInfoTitle[63] = 0;
    wcsncpy(g_nid.szInfo, text.c_str(), 255);
    g_nid.szInfo[255] = 0;
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---------------------------------------------------------------- panel

// Layout metrics in DIPs (scaled by g_scale at draw time).
namespace lay {
    const int PANEL_W = 380;
    const int CARD_MARGIN = 10;   // around each card
    const int CARD_PAD = 14;      // inside each card
    const int HEADER_H = 44;
    const int ROW_H = 34;         // label+resets+pct line and its bar
    const int NOTE_H = 22;
    const int FOOTER_H = 34;
    const int TOGGLE_W = 44;
    const int TOGGLE_H = 22;
}

struct ToggleHit { RECT rc; int num; bool active; };
static std::vector<ToggleHit> g_toggleHits; // panel client coords

static int cardHeight(const Acct& a) {
    int body = a.rows.empty() ? lay::NOTE_H : (int)a.rows.size() * lay::ROW_H;
    if (!a.note.empty()) body = lay::NOTE_H;
    return lay::CARD_PAD * 2 + lay::HEADER_H + body + lay::FOOTER_H;
}

static int panelHeight(const Model& m) {
    if (m.accounts.empty()) return S(70);
    int h = lay::CARD_MARGIN;
    for (const Acct& a : m.accounts) h += cardHeight(a) + lay::CARD_MARGIN;
    return S(h);
}

static void paintPanel(HDC dc, const RECT& client) {
    g_toggleHits.clear();

    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    SolidBrush bgBrush(gpc(C_BG));
    g.FillRectangle(&bgBrush, 0, 0, client.right, client.bottom);

    FontFamily ff(L"Segoe UI");
    Font fTitle(&ff, (REAL)S(12), FontStyleBold, UnitPixel);
    Font fSmall(&ff, (REAL)S(9), FontStyleRegular, UnitPixel);
    Font fRow(&ff, (REAL)S(10), FontStyleRegular, UnitPixel);
    Font fPct(&ff, (REAL)S(10), FontStyleBold, UnitPixel);

    if (g_model.accounts.empty()) {
        gtext(g, fRow, g_haveModel ? L"No managed accounts" : L"Loading…",
              (REAL)client.right / 2, (REAL)S(24), gpc(C_MUTED), GA_CENTER);
    }

    int y = S(lay::CARD_MARGIN);
    for (const Acct& a : g_model.accounts) {
        COLORREF edge = a.active ? C_GREEN_DIM : C_BORDER;
        COLORREF titleColor = a.active ? C_GREEN : C_TEXT;
        int ch = S(cardHeight(a));
        int cardL = S(lay::CARD_MARGIN), cardR = client.right - S(lay::CARD_MARGIN);
        fillRoundRectGp(g, (REAL)cardL, (REAL)y, (REAL)(cardR - cardL), (REAL)ch, (REAL)S(8),
                        gpc(C_CARD), true, gpc(edge));

        int x = cardL + S(lay::CARD_PAD);
        int cy = y + S(lay::CARD_PAD);
        int right = cardR - S(lay::CARD_PAD);

        // header: avatar disc, title + email, active check
        Pen edgePen(gpc(edge), (REAL)S(2));
        g.DrawEllipse(&edgePen, (REAL)x, (REAL)cy, (REAL)S(36), (REAL)S(36));
        gtext(g, fSmall, L"AI", (REAL)(x + S(18)), (REAL)(cy + S(11)), gpc(titleColor), GA_CENTER);

        std::wstring title = a.title + (a.disabled ? L"  · disabled" : L"");
        gtext(g, fTitle, title, (REAL)(x + S(46)), (REAL)(cy + S(1)), gpc(titleColor));
        gtext(g, fSmall, a.email, (REAL)(x + S(46)), (REAL)(cy + S(23)), gpc(C_MUTED));
        if (a.active) {
            SolidBrush green(gpc(C_GREEN));
            g.FillEllipse(&green, (REAL)(right - S(22)), (REAL)(cy + S(2)), (REAL)S(20), (REAL)S(20));
            Pen tick(gpc(C_CARD), (REAL)S(2));
            tick.SetStartCap(LineCapRound);
            tick.SetEndCap(LineCapRound);
            tick.SetLineJoin(LineJoinRound);
            PointF pts[3] = {
                PointF((REAL)(right - S(17)), (REAL)(cy + S(12))),
                PointF((REAL)(right - S(13)), (REAL)(cy + S(16))),
                PointF((REAL)(right - S(7)),  (REAL)(cy + S(9))),
            };
            g.DrawLines(&tick, pts, 3);
        }
        cy += S(lay::HEADER_H);

        // limits
        if (!a.note.empty() || a.rows.empty()) {
            gtext(g, fSmall, a.note.empty() ? L"usage unavailable" : a.note,
                  (REAL)x, (REAL)(cy + S(3)), gpc(C_MUTED));
            cy += S(lay::NOTE_H);
        }
        for (const LimitRow& r : a.rows) {
            wchar_t pctText[16];
            swprintf(pctText, 16, L"%.0f%%", r.pct);
            gtext(g, fRow, r.label, (REAL)x, (REAL)(cy + S(2)), gpc(C_TEXT));
            gtext(g, fPct, pctText, (REAL)right, (REAL)(cy + S(2)), gpc(C_TEXT), GA_RIGHT);
            if (!r.resets.empty()) {
                int pctW = (int)gmeasure(g, fPct, pctText);
                gtext(g, fSmall, L"Resets in " + r.resets,
                      (REAL)(right - pctW - S(10)), (REAL)(cy + S(3)), gpc(C_MUTED), GA_RIGHT);
            }
            // progress bar (rounded)
            REAL barY = (REAL)(cy + S(23)), barW = (REAL)(right - x), barH = (REAL)S(4);
            fillRoundRectGp(g, (REAL)x, barY, barW, barH, barH / 2, gpc(C_BAR_BG));
            if (r.pct > 0) {
                double frac = r.pct > 100 ? 1.0 : r.pct / 100.0;
                REAL fw = (REAL)(barW * frac);
                if (fw < barH) fw = barH;
                fillRoundRectGp(g, (REAL)x, barY, fw, barH, barH / 2, gpc(a.active ? C_GREEN_DIM : C_BLUE));
            }
            cy += S(lay::ROW_H);
        }

        // footer: Active agent + toggle
        cy += S(6);
        gtext(g, fRow, L"Active agent", (REAL)x, (REAL)(cy + S(3)), gpc(titleColor));
        int tw = S(lay::TOGGLE_W), th = S(lay::TOGGLE_H);
        int tgl = right - tw, tgt = cy;
        COLORREF track = a.active ? C_GREEN_DIM : C_KNOB_OFF;
        fillRoundRectGp(g, (REAL)tgl, (REAL)tgt, (REAL)tw, (REAL)th, (REAL)th / 2, gpc(track));
        REAL knob = (REAL)(th - S(4));
        REAL kx = a.active ? (REAL)(tgl + tw) - knob - (REAL)S(2) : (REAL)tgl + (REAL)S(2);
        SolidBrush knobBrush(Color(255, 255, 255, 255));
        g.FillEllipse(&knobBrush, kx, (REAL)(tgt + S(2)), knob, knob);
        gtext(g, fRow, a.title, (REAL)(tgl - S(10)), (REAL)(cy + S(3)), gpc(C_TEXT), GA_RIGHT);
        g_toggleHits.push_back({{tgl, tgt, tgl + tw, tgt + th}, a.num, a.active});

        y += ch + S(lay::CARD_MARGIN);
    }
}

static void switchAccountAsync(int num);

static LRESULT CALLBACK panelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            paintPanel(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (const ToggleHit& t : g_toggleHits)
                if (!t.active && PtInRect(&t.rc, pt)) {
                    switchAccountAsync(t.num);
                    break;
                }
            return 0;
        }
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            for (const ToggleHit& t : g_toggleHits)
                if (!t.active && PtInRect(&t.rc, pt)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Global mouse hook, live only while the panel is open: any button-down
// outside the panel dismisses it. Runs on this thread (we pump messages), so
// it hands off to the main window via a posted message rather than touching
// windows directly.
static LRESULT CALLBACK mouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lp;
        RECT rc;
        GetWindowRect(g_panel, &rc);
        if (!PtInRect(&rc, ms->pt))
            PostMessageW(g_hwnd, WM_HIDEPANEL, 0, 0);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void hidePanel() {
    ShowWindow(g_panel, SW_HIDE);
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    g_panelHiddenTick = GetTickCount();
}

static void showPanel() {
    int w = S(lay::PANEL_W);
    int h = panelHeight(g_model);

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int x = pt.x - w + S(40);
    if (x < mi.rcWork.left + 8) x = mi.rcWork.left + 8;
    if (x + w > mi.rcWork.right - 8) x = mi.rcWork.right - 8 - w;
    int y = pt.y - h - S(16);
    if (y < mi.rcWork.top + 8) y = mi.rcWork.top + 8;

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, S(10), S(10));
    SetWindowRgn(g_panel, rgn, FALSE); // system owns rgn afterwards
    SetWindowPos(g_panel, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    InvalidateRect(g_panel, nullptr, TRUE);
    ShowWindow(g_panel, SW_SHOWNOACTIVATE);
    if (!g_mouseHook)
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHookProc, GetModuleHandleW(nullptr), 0);
}

// ---------------------------------------------------------------- actions

static void refreshSoon() { if (g_refreshNow) SetEvent(g_refreshNow); }

static void switchAccountAsync(int num) {
    CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        int num = (int)(intptr_t)p;
        cswap::Vault v;
        v.load();
        std::string err;
        if (cswap::switchTo(v, num, err)) {
            PostMessageW(g_hwnd, WM_CSWAP_ERR, (WPARAM) new std::wstring(
                L"✓ Switched — restart Claude Code to apply immediately."), 0);
            refreshSoon();
        } else {
            PostMessageW(g_hwnd, WM_CSWAP_ERR, (WPARAM) new std::wstring(
                err.empty() ? L"switch failed" : widen(err)), 0);
        }
        return 0;
    }, (LPVOID)(intptr_t)num, 0, nullptr);
}

// Highest utilization across an account's windows; -1 when unknown.
static double acctMaxPct(const Acct& a) {
    double best = -1;
    for (const LimitRow& r : a.rows) best = r.pct > best ? r.pct : best;
    return best;
}

// Resolve a strategy against the current model. "" rotates to the next
// account, "best" picks the least-used, "next-available" skips any account
// already at its limit. Disabled accounts are never chosen.
static int pickAccount(const wchar_t* strategy) {
    const std::vector<Acct>& accs = g_model.accounts;
    int n = (int)accs.size();
    if (n == 0) return 0;
    int activeIdx = -1;
    for (int i = 0; i < n; ++i)
        if (accs[i].active) activeIdx = i;

    if (!strategy || !*strategy) {  // rotate
        for (int k = 1; k <= n; ++k) {
            const Acct& a = accs[(activeIdx + k) % n];
            if (!a.disabled && !a.active) return a.num;
        }
        return 0;
    }
    if (wcscmp(strategy, L"best") == 0) {
        int pick = 0;
        double bestPct = 1e9;
        for (const Acct& a : accs) {
            if (a.disabled || a.active) continue;
            double p = acctMaxPct(a);
            if (p < 0) p = 50;  // unknown usage: treat as middling, not free
            if (p < bestPct) { bestPct = p; pick = a.num; }
        }
        return pick;
    }
    if (wcscmp(strategy, L"next-available") == 0) {
        for (int k = 1; k <= n; ++k) {
            const Acct& a = accs[(activeIdx + k) % n];
            if (a.disabled || a.active) continue;
            double p = acctMaxPct(a);
            if (p < 100) return a.num;
        }
        return 0;
    }
    return 0;
}

static void switchStrategyAsync(const wchar_t* strategy) {
    int num = pickAccount(strategy);
    if (!num) {
        PostMessageW(g_hwnd, WM_CSWAP_ERR,
                     (WPARAM) new std::wstring(L"No other account available to switch to."), 0);
        return;
    }
    switchAccountAsync(num);
}

// ---- add / remove account ----------------------------------------------------

// Browser login, run entirely on a worker thread so the tray stays responsive.
// The callback page offers a "Copy code" button; because we generated `state`
// ourselves, the clipboard entry ending in "#<state>" is unambiguously ours,
// so the code is consumed automatically — the user never pastes anything.
static DWORD WINAPI addAccountWorker(LPVOID) {
    // Loopback redirect: the browser hands the code straight back to us, so
    // approving in the browser is the only thing the user has to do.
    cswap::Loopback lb;
    if (!cswap::loopbackStart(lb)) {
        PostMessageW(g_hwnd, WM_CSWAP_ERR, (WPARAM) new std::wstring(
            L"Could not open a local callback port."), 0);
        return 0;
    }
    std::string redirect = cswap::loopbackRedirectUri(lb);

    cswap::Pkce pk = cswap::makePkce();
    std::string url = cswap::authorizeUrl(pk, redirect);
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    PostMessageW(g_hwnd, WM_CSWAP_ERR, (WPARAM) new std::wstring(
        L"Sign in and press Authorize — the account is added automatically."), 0);

    std::string code = cswap::awaitAuthCode(lb, pk, 300, &g_running);
    cswap::loopbackStop(lb);
    if (code.empty()) {
        PostMessageW(g_hwnd, WM_CSWAP_ERR, (WPARAM) new std::wstring(
            L"Add account timed out — no authorization was received."), 0);
        return 0;
    }

    cswap::OAuthCred cred;
    std::string err;
    if (!cswap::exchangeCode(code, pk, redirect, cred, err)) {
        PostMessageW(g_hwnd, WM_CSWAP_ERR, (WPARAM) new std::wstring(
            L"Sign-in failed: " + widen(err)), 0);
        return 0;
    }
    cswap::Vault v;
    v.load();
    int num = cswap::storeLogin(v, cred, err);
    if (!num) {
        PostMessageW(g_hwnd, WM_CSWAP_ERR, (WPARAM) new std::wstring(
            L"Could not save the account: " + widen(err)), 0);
        return 0;
    }
    PostMessageW(g_hwnd, WM_CSWAP_ERR, (WPARAM) new std::wstring(
        L"Added account " + std::to_wstring(num) + L" (" + widen(v.byNum(num)->email) + L")"), 0);
    refreshSoon();
    return 0;
}

static void addAccountAsync() {
    CreateThread(nullptr, 0, addAccountWorker, nullptr, 0, nullptr);
}

static void removeAccount(int num) {
    cswap::VaultLock lock;
    cswap::Vault v;
    v.load();
    cswap::Account* a = v.byNum(num);
    if (!a) return;
    std::wstring msg = L"Remove account " + std::to_wstring(num) + L" (" + widen(a->email)
                     + L")?\n\nThe stored credential is deleted. Your Claude Code login is "
                       L"not signed out.";
    if (MessageBoxW(nullptr, msg.c_str(), APP_NAME,
                    MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST) != IDOK)
        return;
    v.removeAccount(num);
    if (v.save()) {
        g_usageCache.erase(num);
        refreshSoon();
    } else {
        notify(APP_NAME, L"Could not update the account store.");
    }
}

// ---- autostart (HKCU Run) ----------------------------------------------------

static bool autostartEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    bool found = RegQueryValueExW(key, AUTOSTART_VALUE, nullptr, nullptr, nullptr, nullptr)
                 == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

static void setAutostart(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, AUTOSTART_VALUE, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, AUTOSTART_VALUE);
    }
    RegCloseKey(key);
}

// ---------------------------------------------------------------- menu

enum {
    IDM_ACCOUNT_BASE = 100,  // + account number
    IDM_ROTATE = 200,
    IDM_BEST = 201,
    IDM_NEXT_AVAILABLE = 202,
    IDM_REFRESH = 210,
    IDM_AUTOSTART = 220,
    IDM_OPEN_LOG = 230,
    IDM_QUIT = 240,
    IDM_ADD_ACCOUNT = 250,
    IDM_ABOUT = 260,
    IDM_REMOVE_BASE = 300,   // + account number
};

static void showAbout() {
    std::wstring text =
        std::wstring(APP_NAME) + L"  " + APP_VERSION + L"\n\n"
        L"Multi-account switcher for Claude Code.\n"
        L"A C++ fork of claude-swap.\n\n"
        L"Bilyk Ihor\n" +
        APP_URL + L"\n"
        L"MIT License";
    MessageBoxW(nullptr, text.c_str(), (std::wstring(L"About ") + APP_NAME).c_str(),
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

static void showMenu() {
    HMENU menu = CreatePopupMenu();
    if (g_model.accounts.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No managed accounts");
    }
    for (const Acct& a : g_model.accounts) {
        std::wstring label = std::to_wstring(a.num) + L"  " + a.title;
        if (!a.rows.empty()) {
            wchar_t pct[48];
            swprintf(pct, 48, L"   — %.0f%%", a.rows[0].pct);
            label += pct;
        }
        if (a.disabled) label += L"  (disabled)";
        UINT flags = MF_STRING | (a.active ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, IDM_ACCOUNT_BASE + a.num, label.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_ROTATE, L"Rotate to next");
    AppendMenuW(menu, MF_STRING, IDM_BEST, L"Switch to best");
    AppendMenuW(menu, MF_STRING, IDM_NEXT_AVAILABLE, L"Next available");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_ADD_ACCOUNT, L"Add account…");
    if (!g_model.accounts.empty()) {
        HMENU removeMenu = CreatePopupMenu();
        for (const Acct& a : g_model.accounts)
            AppendMenuW(removeMenu, MF_STRING, IDM_REMOVE_BASE + a.num,
                        (std::to_wstring(a.num) + L"  " + a.email).c_str());
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)removeMenu, L"Remove account");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"Refresh now");
    AppendMenuW(menu, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART,
                L"Start with Windows");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_LOG, L"Open data folder…");
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"About…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd); // required for the menu to dismiss correctly
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
}

static void onCommand(int id) {
    if (id >= IDM_ACCOUNT_BASE && id < IDM_ROTATE) {
        switchAccountAsync(id - IDM_ACCOUNT_BASE);
        return;
    }
    if (id >= IDM_REMOVE_BASE) {
        removeAccount(id - IDM_REMOVE_BASE);
        return;
    }
    switch (id) {
        case IDM_ROTATE: switchStrategyAsync(L""); break;
        case IDM_BEST: switchStrategyAsync(L"best"); break;
        case IDM_NEXT_AVAILABLE: switchStrategyAsync(L"next-available"); break;
        case IDM_ADD_ACCOUNT: addAccountAsync(); break;
        case IDM_ABOUT: showAbout(); break;
        case IDM_REFRESH: refreshSoon(); break;
        case IDM_AUTOSTART: setAutostart(!autostartEnabled()); break;
        case IDM_OPEN_LOG:
            // The account store (DPAPI-encrypted credentials + metadata).
            ShellExecuteW(nullptr, L"open", cswap::vaultDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDM_QUIT:
            g_running = false;
            refreshSoon();
            DestroyWindow(g_hwnd);
            break;
    }
}

// ---------------------------------------------------------------- refresh

static DWORD WINAPI refreshLoop(LPVOID) {
    while (g_running) {
        Model* m = new Model(buildModel());
        PostMessageW(g_hwnd, WM_MODEL, (WPARAM)m, 0);
        WaitForSingleObject(g_refreshNow, g_refreshSec * 1000);
        ResetEvent(g_refreshNow);
    }
    return 0;
}

// ---------------------------------------------------------------- main window

static LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {
        Shell_NotifyIconW(NIM_ADD, &g_nid); // explorer restarted: re-register
        updateTray();
        return 0;
    }
    switch (msg) {
        case WM_TRAYCB:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                    // Click toggles the panel. When it's open, the mouse hook
                    // has already posted WM_HIDEPANEL for this same button
                    // press (fired on button-down, before this button-up), so
                    // the panel is now hidden with a fresh g_panelHiddenTick;
                    // the debounce keeps the click from immediately reopening.
                    if (!IsWindowVisible(g_panel) && GetTickCount() - g_panelHiddenTick > 250)
                        showPanel();
                    break;
                case WM_RBUTTONUP:
                    showMenu();
                    break;
            }
            return 0;
        case WM_HIDEPANEL:
            hidePanel();
            return 0;
        case WM_MODEL: {
            Model* m = (Model*)wp;
            g_model = std::move(*m);
            delete m;
            g_haveModel = true;
            updateTray();
            if (IsWindowVisible(g_panel)) {
                // re-show to adopt the (possibly changed) height
                InvalidateRect(g_panel, nullptr, TRUE);
                int w = S(lay::PANEL_W), h = panelHeight(g_model);
                RECT rc;
                GetWindowRect(g_panel, &rc);
                HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, S(10), S(10));
                SetWindowRgn(g_panel, rgn, FALSE);
                SetWindowPos(g_panel, HWND_TOPMOST, rc.left, rc.bottom - h, w, h, SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_CSWAP_ERR: {
            std::wstring* s = (std::wstring*)wp;
            notify(APP_NAME, *s);
            delete s;
            return 0;
        }
        case WM_COMMAND:
            onCommand(LOWORD(wp));
            return 0;
        case WM_DESTROY:
            if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- entry

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // second launch just pings the first instance's icon alive and exits
    CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    g_scale = GetDpiForSystem() / 96.0;

    GdiplusStartupInput gdipInput;
    GdiplusStartup(&g_gdipToken, &gdipInput, nullptr);

    // optional config: {"refreshSec": 60}
    std::string cfg = readFileUtf8(backupDir() + L"\\tray_native.json");
    if (!cfg.empty()) {
        JParser parser(cfg);
        JVal root = parser.parse();
        if (root.get("refreshSec") && root.get("refreshSec")->type == JVal::NUM) {
            int v = (int)root.get("refreshSec")->num;
            if (v >= 15 && v <= 3600) g_refreshSec = v;
        }
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = mainProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"CswapTrayMain";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, inst, nullptr);

    WNDCLASSW pc = {};
    pc.lpfnWndProc = panelProc;
    pc.hInstance = inst;
    pc.lpszClassName = L"CswapTrayPanel";
    pc.hbrBackground = nullptr;
    RegisterClassW(&pc);
    g_panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              pc.lpszClassName, L"", WS_POPUP, 0, 0, S(lay::PANEL_W), S(200),
                              nullptr, nullptr, inst, nullptr);

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYCB;
    g_nid.hIcon = makeIcon(-1);
    wcscpy(g_nid.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_refreshNow = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, refreshLoop, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GdiplusShutdown(g_gdipToken);
    return 0;
}
