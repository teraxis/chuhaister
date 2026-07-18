// OAuth + usage API for the cswap native core (mirrors claude_swap.oauth).
//
// Endpoints (same ones Claude Code / the Python fork use):
//   POST platform.claude.com/v1/oauth/token    — refresh_token grant
//   GET  api.anthropic.com/api/oauth/profile   — token -> account identity
//   GET  api.anthropic.com/api/oauth/usage     — 5h/7d/per-model utilization

#pragma once

#include "http.h"
#include "json.h"

#include <ctime>
#include <string>
#include <vector>

namespace cswap {

static const char* OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
static const wchar_t* OAUTH_TOKEN_URL = L"https://platform.claude.com/v1/oauth/token";
static const wchar_t* PROFILE_URL = L"https://api.anthropic.com/api/oauth/profile";
static const wchar_t* USAGE_URL = L"https://api.anthropic.com/api/oauth/usage";
static const wchar_t* OAUTH_BETA_HEADER = L"anthropic-beta: oauth-2025-04-20";

// ---- credential payload ------------------------------------------------------

// The claudeAiOauth object of ~/.claude/.credentials.json.
struct OAuthCred {
    std::string accessToken;
    std::string refreshToken;
    long long expiresAtMs = 0;
    std::vector<std::string> scopes;
    std::string subscriptionType; // passthrough, may be empty
    bool valid = false;
};

inline OAuthCred parseCredentials(const std::string& credJson) {
    OAuthCred c;
    JParser p(credJson);
    JVal root = p.parse();
    const JVal* o = root.get("claudeAiOauth");
    if (!p.ok || !o || o->type != JVal::OBJ) return c;
    c.accessToken = o->get("accessToken") ? o->get("accessToken")->strOr("") : "";
    c.refreshToken = o->get("refreshToken") ? o->get("refreshToken")->strOr("") : "";
    c.expiresAtMs = (long long)(o->get("expiresAt") ? o->get("expiresAt")->numOr(0) : 0);
    if (const JVal* sc = o->get("scopes"); sc && sc->type == JVal::ARR)
        for (const JVal& s : sc->arr)
            if (s.type == JVal::STR) c.scopes.push_back(s.str);
    if (const JVal* st = o->get("subscriptionType"); st && st->type == JVal::STR)
        c.subscriptionType = st->str;
    c.valid = !c.accessToken.empty();
    return c;
}

inline std::string serializeCredentials(const OAuthCred& c) {
    std::string scopes;
    for (size_t i = 0; i < c.scopes.size(); ++i)
        scopes += (i ? "," : "") + jsonString(c.scopes[i]);
    std::string out = "{\"claudeAiOauth\":{";
    out += "\"accessToken\":" + jsonString(c.accessToken);
    if (!c.refreshToken.empty()) out += ",\"refreshToken\":" + jsonString(c.refreshToken);
    if (c.expiresAtMs > 0) out += ",\"expiresAt\":" + std::to_string(c.expiresAtMs);
    out += ",\"scopes\":[" + scopes + "]";
    if (!c.subscriptionType.empty())
        out += ",\"subscriptionType\":" + jsonString(c.subscriptionType);
    out += "}}";
    return out;
}

// Expired or about to expire (5-minute buffer, mirrors the Python fork).
inline bool tokenExpired(const OAuthCred& c) {
    if (c.expiresAtMs <= 0) return false; // setup-tokens carry no expiry
    long long nowMs = (long long)time(nullptr) * 1000;
    return nowMs >= c.expiresAtMs - 5 * 60 * 1000;
}

// ---- refresh grant -----------------------------------------------------------

enum class RefreshError { None, NoRefreshToken, InvalidGrant, Transient };

// POST the refresh grant; on success updates `c` in place (access token,
// expiry, rotated refresh token, scopes).
inline RefreshError refreshCredentials(OAuthCred& c) {
    if (c.refreshToken.empty()) return RefreshError::NoRefreshToken;
    std::string body = "{\"grant_type\":\"refresh_token\",\"refresh_token\":"
                       + jsonString(c.refreshToken) + ",\"client_id\":"
                       + jsonString(OAUTH_CLIENT_ID) + "}";
    HttpResponse r = httpsRequest(L"POST", OAUTH_TOKEN_URL,
                                  {L"Content-Type: application/json"}, body);
    if (!r.ok) return RefreshError::Transient;
    if (r.status != 200) {
        bool permanent = (r.status == 400 || r.status == 401 || r.status == 403)
                         && (r.body.find("invalid_grant") != std::string::npos
                             || r.body.find("invalid_client") != std::string::npos);
        return permanent ? RefreshError::InvalidGrant : RefreshError::Transient;
    }
    JParser p(r.body);
    JVal root = p.parse();
    if (!p.ok || !root.get("access_token")) return RefreshError::Transient;
    c.accessToken = root.get("access_token")->strOr("");
    long long nowMs = (long long)time(nullptr) * 1000;
    c.expiresAtMs = nowMs + (long long)(root.get("expires_in") ? root.get("expires_in")->numOr(0) : 0) * 1000;
    if (const JVal* rt = root.get("refresh_token"); rt && rt->type == JVal::STR && !rt->str.empty())
        c.refreshToken = rt->str;
    if (const JVal* sc = root.get("scope"); sc && sc->type == JVal::STR) {
        c.scopes.clear();
        std::string cur;
        for (char ch : sc->str + " ") {
            if (ch == ' ') { if (!cur.empty()) c.scopes.push_back(cur); cur.clear(); }
            else cur += ch;
        }
    }
    return RefreshError::None;
}

// ---- profile -----------------------------------------------------------------

struct Profile {
    bool ok = false;
    std::string email, accountUuid, orgUuid, orgName;
};

inline Profile fetchProfile(const std::string& accessToken) {
    Profile out;
    HttpResponse r = httpsRequest(L"GET", PROFILE_URL,
                                  {L"Authorization: Bearer " + widen(accessToken),
                                   L"Content-Type: application/json"},
                                  "");
    if (!r.ok || r.status != 200) return out;
    JParser p(r.body);
    JVal root = p.parse();
    const JVal* acc = root.get("account");
    if (!p.ok || !acc || acc->type != JVal::OBJ) return out;
    out.email = acc->get("email") ? acc->get("email")->strOr("") : "";
    out.accountUuid = acc->get("uuid") ? acc->get("uuid")->strOr("") : "";
    if (const JVal* org = root.get("organization"); org && org->type == JVal::OBJ) {
        out.orgUuid = org->get("uuid") ? org->get("uuid")->strOr("") : "";
        out.orgName = org->get("name") ? org->get("name")->strOr("") : "";
    }
    out.ok = !out.email.empty();
    return out;
}

// ---- usage -------------------------------------------------------------------

struct UsageWindow {
    bool present = false;
    double pct = 0;
    std::string resetsAt; // ISO 8601, may be empty
};

struct ScopedWindow {
    std::string name; // model display name, e.g. "Fable"
    double pct = 0;
    std::string resetsAt;
};

struct Usage {
    bool ok = false;
    DWORD httpStatus = 0;
    UsageWindow fiveHour, sevenDay;
    std::vector<ScopedWindow> scoped;
};

inline Usage fetchUsage(const std::string& accessToken) {
    Usage out;
    HttpResponse r = httpsRequest(L"GET", USAGE_URL,
                                  {L"Authorization: Bearer " + widen(accessToken),
                                   OAUTH_BETA_HEADER},
                                  "");
    out.httpStatus = r.status;
    if (!r.ok || r.status != 200) return out;
    JParser p(r.body);
    JVal root = p.parse();
    if (!p.ok || root.type != JVal::OBJ) return out;

    auto window = [](const JVal* w, UsageWindow& dst) {
        if (!w || w->type != JVal::OBJ) return;
        const JVal* u = w->get("utilization");
        if (!u || u->type != JVal::NUM) return;
        dst.present = true;
        dst.pct = u->num;
        if (const JVal* ra = w->get("resets_at"); ra && ra->type == JVal::STR)
            dst.resetsAt = ra->str;
    };
    window(root.get("five_hour"), out.fiveHour);
    window(root.get("seven_day"), out.sevenDay);

    // Per-model weekly limits: limits[] entries with scope.model.display_name.
    if (const JVal* limits = root.get("limits"); limits && limits->type == JVal::ARR)
        for (const JVal& lim : limits->arr) {
            if (lim.type != JVal::OBJ) continue;
            const JVal* model = lim.get2("scope", "model");
            const JVal* name = model ? model->get("display_name") : nullptr;
            const JVal* pct = lim.get("percent");
            if (!name || name->type != JVal::STR || !pct || pct->type != JVal::NUM) continue;
            ScopedWindow sw;
            sw.name = name->str;
            sw.pct = pct->num;
            if (const JVal* ra = lim.get("resets_at"); ra && ra->type == JVal::STR)
                sw.resetsAt = ra->str;
            out.scoped.push_back(sw);
        }

    out.ok = true;
    return out;
}

} // namespace cswap
