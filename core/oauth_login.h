// Browser OAuth login (authorization-code + PKCE) for the cswap native core.
//
// This is the flow `claude setup-token` uses — the Python fork never
// implemented it (it only accepts an already-minted token), so the endpoints
// and parameters here come from the documented Claude Code OAuth flow, not
// from the fork's code. Verified live before relying on it.
//
//   1. pkce()          -> {verifier, challenge}  (S256)
//   2. authorizeUrl()  -> open in the browser; user logs in at claude.ai and
//                         approves; the callback page shows a code as CODE#STATE
//   3. exchangeCode()  -> POST the code + verifier to the token endpoint,
//                         yielding a full OAuth credential (access+refresh)
//
// Then fetchProfile() (oauth.h) resolves the token to email/org for the vault.

#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include "http.h"
#include "json.h"
#include "oauth.h"
#include "vault.h"

#include <bcrypt.h>
#include <shellapi.h>

#include <cctype>
#include <ctime>
#include <string>

namespace cswap {

// Captured verbatim from Claude Code's own `/login` authorize URL — the only
// reliable source (the binary's string table alone was not enough). Getting
// any of these wrong makes the org-scoped POST /v1/oauth/{orgUuid}/authorize
// answer 400 "Invalid request format" *after* the consent screen:
//   * host must be claude.com/cai (not claude.ai, not platform.claude.com)
//   * redirect is the manual page (loopback is rejected for this flow)
//   * ALL SIX scopes are required — omitting the claude_code/mcp/file_upload
//     ones was what actually broke every earlier attempt
static const char* AUTHORIZE_BASE = "https://claude.com/cai/oauth/authorize";
static const char* OAUTH_REDIRECT_URI = "https://platform.claude.com/oauth/code/callback";
static const char* OAUTH_LOGIN_SCOPES =
    "org:create_api_key user:profile user:inference "
    "user:sessions:claude_code user:mcp_servers user:file_upload";

// ---- primitives (Windows CNG) ------------------------------------------------

inline std::string base64url(const unsigned char* data, size_t len) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        unsigned n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += t[(n >> 18) & 63]; out += t[(n >> 12) & 63];
        out += t[(n >> 6) & 63];  out += t[n & 63];
    }
    if (len - i == 1) {
        unsigned n = data[i] << 16;
        out += t[(n >> 18) & 63]; out += t[(n >> 12) & 63];
    } else if (len - i == 2) {
        unsigned n = (data[i] << 16) | (data[i + 1] << 8);
        out += t[(n >> 18) & 63]; out += t[(n >> 12) & 63]; out += t[(n >> 6) & 63];
    }
    return out;
}

inline bool randomBytes(unsigned char* buf, size_t len) {
    return BCryptGenRandom(nullptr, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

inline bool sha256(const std::string& in, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    NTSTATUS st = BCryptHash(alg, nullptr, 0, (PUCHAR)in.data(), (ULONG)in.size(), out, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    return st == 0;
}

inline std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// ---- PKCE + authorize URL ----------------------------------------------------

struct Pkce { std::string verifier, challenge, state; };

inline Pkce makePkce() {
    Pkce p;
    // Both verifier and state are 32 random bytes -> 43-char base64url, which
    // is what Claude Code sends. A shorter state (16 bytes / 22 chars) is
    // rejected by the org-scoped authorize POST as "Invalid request format".
    unsigned char v[32], s[32];
    randomBytes(v, sizeof(v));
    randomBytes(s, sizeof(s));
    p.verifier = base64url(v, sizeof(v));
    unsigned char hash[32];
    sha256(p.verifier, hash);
    p.challenge = base64url(hash, sizeof(hash));
    p.state = base64url(s, sizeof(s));
    return p;
}

// `redirectUri` defaults to the manual page; the loopback flow passes its own
// http://localhost:PORT/callback. Whatever is used here MUST be repeated
// verbatim in exchangeCode (the token endpoint checks they match).
// Space-separated scopes -> '+'-joined, each token percent-encoded. Claude
// Code sends '+' (form encoding) rather than %20; mirror it exactly.
inline std::string encodeScopes(const std::string& scopes) {
    std::string out, tok;
    for (size_t i = 0; i <= scopes.size(); ++i) {
        if (i == scopes.size() || scopes[i] == ' ') {
            if (!tok.empty()) {
                if (!out.empty()) out += '+';
                out += urlEncode(tok);
                tok.clear();
            }
        } else {
            tok += scopes[i];
        }
    }
    return out;
}

inline std::string authorizeUrl(const Pkce& p, const std::string& redirectUri = OAUTH_REDIRECT_URI) {
    std::string u = std::string(AUTHORIZE_BASE) + "?code=true";
    u += "&client_id=" + std::string(OAUTH_CLIENT_ID);
    u += "&response_type=code";
    u += "&redirect_uri=" + urlEncode(redirectUri);
    u += "&scope=" + encodeScopes(OAUTH_LOGIN_SCOPES);
    u += "&code_challenge=" + p.challenge;
    u += "&code_challenge_method=S256";
    u += "&state=" + p.state;
    return u;
}

// ---- loopback redirect capture (RFC 8252 native-app flow) --------------------

// Listens on BOTH loopback families at the same port. "localhost" resolves to
// ::1 before 127.0.0.1 on Windows, so an IPv4-only listener can leave the
// browser unable to reach the callback; binding both removes that race.
struct Loopback {
    SOCKET v4 = INVALID_SOCKET;
    SOCKET v6 = INVALID_SOCKET;
    int port = 0;
    bool wsa = false;
};

inline bool loopbackStart(Loopback& lb) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    lb.wsa = true;

    lb.v4 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lb.v4 == INVALID_SOCKET) return false;
    sockaddr_in a4 = {};
    a4.sin_family = AF_INET;
    a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a4.sin_port = 0;  // OS picks a free port
    if (bind(lb.v4, (sockaddr*)&a4, sizeof(a4)) != 0) return false;
    int len = sizeof(a4);
    if (getsockname(lb.v4, (sockaddr*)&a4, &len) != 0) return false;
    lb.port = ntohs(a4.sin_port);
    if (listen(lb.v4, 4) != 0) return false;

    // Mirror it on ::1. Best-effort: if IPv6 is unavailable the IPv4 listener
    // alone still works (browsers fall back).
    lb.v6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (lb.v6 != INVALID_SOCKET) {
        int on = 1;
        setsockopt(lb.v6, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&on, sizeof(on));
        sockaddr_in6 a6 = {};
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_loopback;
        a6.sin6_port = htons((u_short)lb.port);
        if (bind(lb.v6, (sockaddr*)&a6, sizeof(a6)) != 0 || listen(lb.v6, 4) != 0) {
            closesocket(lb.v6);
            lb.v6 = INVALID_SOCKET;
        }
    }
    return true;
}

// Must say "localhost": the token endpoint rejects a 127.0.0.1 redirect_uri
// ("Invalid 'redirect_uri' in request") even though the authorize step will
// happily redirect there.
inline std::string loopbackRedirectUri(const Loopback& lb) {
    return "http://localhost:" + std::to_string(lb.port) + "/callback";
}

// One short wait for the browser's callback. Returns false if nothing arrived
// within `timeoutMs`, so callers can interleave other checks (e.g. clipboard)
// instead of blocking on the socket alone.
inline bool loopbackPoll(Loopback& lb, std::string& code, std::string& state, int timeoutMs = 250) {
    fd_set fds;
    FD_ZERO(&fds);
    if (lb.v4 != INVALID_SOCKET) FD_SET(lb.v4, &fds);
    if (lb.v6 != INVALID_SOCKET) FD_SET(lb.v6, &fds);
    timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, &fds, nullptr, nullptr, &tv) <= 0) return false;
    SOCKET ready = (lb.v4 != INVALID_SOCKET && FD_ISSET(lb.v4, &fds)) ? lb.v4 : lb.v6;
    if (ready == INVALID_SOCKET) return false;
    SOCKET client = accept(ready, nullptr, nullptr);
    if (client == INVALID_SOCKET) return false;

    std::string req;
    char buf[4096];
    int n = recv(client, buf, sizeof(buf) - 1, 0);
    if (n > 0) req.assign(buf, n);

    auto qparam = [&](const std::string& key) -> std::string {
        size_t p = req.find(key + "=");
        if (p == std::string::npos) return "";
        p += key.size() + 1;
        size_t e = req.find_first_of("& \r\n", p);
        return req.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };
    code = qparam("code");
    state = qparam("state");

    static const char* page =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
        "<!doctype html><html><body style=\"font-family:Segoe UI,system-ui;background:#161616;"
        "color:#e6e6e6;text-align:center;padding-top:90px\">"
        "<h2 style=\"color:#3fb950\">Account added to cswap</h2>"
        "<p>You can close this window and return to the app.</p></body></html>";
    send(client, page, (int)strlen(page), 0);
    closesocket(client);
    return !code.empty();
}

inline void loopbackStop(Loopback& lb) {
    if (lb.v4 != INVALID_SOCKET) { closesocket(lb.v4); lb.v4 = INVALID_SOCKET; }
    if (lb.v6 != INVALID_SOCKET) { closesocket(lb.v6); lb.v6 = INVALID_SOCKET; }
    if (lb.wsa) { WSACleanup(); lb.wsa = false; }
}


// ---- clipboard pickup --------------------------------------------------------

inline bool readClipboardText(std::string& out) {
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        wchar_t* p = (wchar_t*)GlobalLock(h);
        if (p) { out = narrow(p); ok = true; GlobalUnlock(h); }
    }
    CloseClipboard();
    return ok;
}

// The callback page's "Copy code" button puts CODE#STATE on the clipboard.
// Since we generated `state` ourselves, an entry ending in "#<state>" is
// unambiguously the code for *this* login — so it can be consumed without
// asking the user to paste anything.
inline bool clipboardHasOurCode(const std::string& state, std::string& code) {
    std::string clip;
    if (!readClipboardText(clip)) return false;
    while (!clip.empty() && isspace((unsigned char)clip.back())) clip.pop_back();
    while (!clip.empty() && isspace((unsigned char)clip.front())) clip.erase(clip.begin());
    size_t h = clip.find('#');
    if (h == std::string::npos || clip.substr(h + 1) != state) return false;
    code = clip;
    return true;
}

// Wait for the authorization code by whichever route arrives first:
//   * the browser redirecting to our loopback server — fully automatic, and
//     what Claude Code's own oauth_callback_listener does, or
//   * the user clicking "Copy code" on the manual callback page — the safety
//     net if this flow ever refuses a loopback redirect.
// Returns the raw code (bare, or CODE#STATE from the clipboard), or "" on
// timeout. `running` is a keep-going flag (not a cancel flag): the wait stops
// early once it turns false, e.g. when the app is quitting.
inline std::string awaitAuthCode(Loopback& lb, const Pkce& pk, int timeoutSec = 300,
                                 const volatile bool* running = nullptr) {
    std::string code, state;
    for (int i = 0; i < timeoutSec * 4; ++i) {
        if (running && !*running) break;
        if (loopbackPoll(lb, code, state, 250) && !code.empty()) return code;
        if (clipboardHasOurCode(pk.state, code)) return code;
    }
    return "";
}

// ---- code exchange -----------------------------------------------------------

// Pull code & state out of whatever the user pasted: a full callback URL
// (…/callback?code=X&state=Y), a "CODE#STATE" pair, or a bare code (then the
// PKCE state is assumed). Robust to surrounding whitespace.
inline void parseCodeInput(const std::string& input, const std::string& fallbackState,
                           std::string& code, std::string& state) {
    std::string s = input;
    auto trim = [](std::string& x) {
        while (!x.empty() && (x.front() == ' ' || x.front() == '\r' || x.front() == '\n' || x.front() == '\t')) x.erase(x.begin());
        while (!x.empty() && (x.back() == ' ' || x.back() == '\r' || x.back() == '\n' || x.back() == '\t')) x.pop_back();
    };
    trim(s);
    if (s.find("code=") != std::string::npos) {  // a URL or query string
        auto qp = [&](const std::string& key) -> std::string {
            size_t p = s.find(key + "=");
            if (p == std::string::npos) return "";
            p += key.size() + 1;
            size_t e = s.find_first_of("&# \r\n", p);
            return s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        };
        code = qp("code");
        state = qp("state");
        if (state.empty()) state = fallbackState;
    } else if (size_t h = s.find('#'); h != std::string::npos) {  // CODE#STATE
        code = s.substr(0, h);
        state = s.substr(h + 1);
    } else {  // bare code
        code = s;
        state = fallbackState;
    }
    trim(code);
    trim(state);
}

// Exchange the authorization code for tokens. `pastedCode` may be a full
// callback URL, "CODE#STATE", or a bare code. `redirectUri` MUST equal the one
// sent to the authorize endpoint.
inline bool exchangeCode(const std::string& pastedCode, const Pkce& p,
                         const std::string& redirectUri, OAuthCred& out, std::string& err) {
    std::string code, state;
    parseCodeInput(pastedCode, p.state, code, state);

    std::string body = "{\"grant_type\":\"authorization_code\"";
    body += ",\"code\":" + jsonString(code);
    body += ",\"state\":" + jsonString(state);
    body += ",\"client_id\":" + jsonString(OAUTH_CLIENT_ID);
    body += ",\"redirect_uri\":" + jsonString(redirectUri);
    body += ",\"code_verifier\":" + jsonString(p.verifier) + "}";

    HttpResponse r = httpsRequest(L"POST", OAUTH_TOKEN_URL,
                                  {L"Content-Type: application/json"}, body);
    if (!r.ok) { err = "network error contacting token endpoint"; return false; }
    if (r.status != 200) {
        err = "token endpoint returned HTTP " + std::to_string(r.status)
            + ": " + r.body.substr(0, 300);
        return false;
    }
    JParser jp(r.body);
    JVal root = jp.parse();
    if (!jp.ok || !root.get("access_token")) { err = "no access_token in response"; return false; }
    out.accessToken = root.get("access_token")->strOr("");
    if (const JVal* rt = root.get("refresh_token"); rt && rt->type == JVal::STR)
        out.refreshToken = rt->str;
    long long nowMs = (long long)time(nullptr) * 1000;
    out.expiresAtMs = nowMs + (long long)(root.get("expires_in") ? root.get("expires_in")->numOr(0) : 0) * 1000;
    out.scopes.clear();
    if (const JVal* sc = root.get("scope"); sc && sc->type == JVal::STR) {
        std::string cur;
        for (char ch : sc->str + " ") {
            if (ch == ' ') { if (!cur.empty()) out.scopes.push_back(cur); cur.clear(); }
            else cur += ch;
        }
    }
    out.valid = !out.accessToken.empty();
    if (!out.valid) err = "empty access token in response";
    return out.valid;
}

// Store a freshly-minted login as a vault account (add or update-in-place).
// The identity comes from the OAuth profile endpoint. Returns account num or 0.
inline int storeLogin(Vault& v, const OAuthCred& cred, std::string& err) {
    Profile pr = fetchProfile(cred.accessToken);
    if (!pr.ok) { err = "could not resolve account identity (profile fetch failed)"; return 0; }
    // Re-read under the lock: another process may have changed the vault while
    // the browser round-trip was in flight.
    VaultLock lock;
    v.load();
    Account* existing = v.byEmail(pr.email);
    Account acc;
    if (existing) acc = *existing;
    else acc.num = v.nextNum();
    acc.email = pr.email;
    acc.accountUuid = pr.accountUuid;
    acc.orgUuid = pr.orgUuid;
    acc.orgName = pr.orgName;
    if (!v.writeCred(acc.num, serializeCredentials(cred))) { err = "DPAPI write failed"; return 0; }
    if (existing) *existing = acc;
    else v.accounts.push_back(acc);
    if (!v.save()) { err = "vault metadata write failed"; return 0; }
    return acc.num;
}

} // namespace cswap
