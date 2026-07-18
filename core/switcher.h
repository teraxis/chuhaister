// Live Claude Code state + the switch operation, for the cswap native core.
//
// Reads/writes the same files Claude Code itself uses:
//   ~/.claude/.credentials.json  — the active OAuth credential (whole file ours)
//   ~/.claude.json               — huge config; we touch ONLY the top-level
//                                  "oauthAccount" object, via byte-precise
//                                  splice (json.h) + atomic replace, so none
//                                  of Claude Code's other state is disturbed.
//
// Switch algorithm (essential subset of the Python fork's):
//   1. read live credential + identity
//   2. if the live identity matches a managed account -> refresh its vault
//      copy (the live token may have rotated since the last switch)
//   3. write the target's credential file
//   4. splice the target's oauthAccount into ~/.claude.json

#pragma once

#include "oauth.h"
#include "vault.h"

namespace cswap {

struct LiveState {
    bool haveCred = false;
    std::string credJson;      // raw ~/.claude/.credentials.json content
    bool haveIdentity = false; // oauthAccount present in ~/.claude.json
    std::string email, accountUuid, orgUuid, orgName;
    std::string rawOauthAccount; // the oauthAccount object's exact source text
};

inline LiveState readLiveState() {
    LiveState s;
    s.haveCred = readFile(credentialsPath(), s.credJson) && !s.credJson.empty();
    std::string cfg;
    if (readFile(globalConfigPath(), cfg)) {
        // Capture the oauthAccount object's *exact* bytes (Claude Code keeps
        // rich profile fields there that a switch must restore verbatim).
        auto span = findTopLevelValue(cfg, "oauthAccount");
        if (span.first != std::string::npos)
            s.rawOauthAccount = cfg.substr(span.first, span.second - span.first);
        JParser p(cfg);
        JVal root = p.parse();
        if (const JVal* oa = root.get("oauthAccount"); p.ok && oa && oa->type == JVal::OBJ) {
            s.email = oa->get("emailAddress") ? oa->get("emailAddress")->strOr("") : "";
            s.accountUuid = oa->get("accountUuid") ? oa->get("accountUuid")->strOr("") : "";
            if (const JVal* u = oa->get("organizationUuid"); u && u->type == JVal::STR)
                s.orgUuid = u->str;
            if (const JVal* n = oa->get("organizationName"); n && n->type == JVal::STR)
                s.orgName = n->str;
            s.haveIdentity = !s.email.empty();
        }
    }
    return s;
}

// Minimal oauthAccount object — the fallback when the vault has no stored raw
// copy (e.g. a setup-token account that never had a live profile). Claude Code
// tolerates the extra fields being absent; it re-fetches them on next use.
inline std::string synthOauthAccount(const Account& a) {
    std::string out = "{\n    \"accountUuid\": " + jsonString(a.accountUuid)
                    + ",\n    \"emailAddress\": " + jsonString(a.email);
    out += ",\n    \"organizationUuid\": "
         + (a.orgUuid.empty() ? std::string("null") : jsonString(a.orgUuid));
    out += ",\n    \"organizationName\": "
         + (a.orgName.empty() ? std::string("null") : jsonString(a.orgName));
    out += "\n  }";
    return out;
}

// Splice `a`'s identity into ~/.claude.json (only that key changes). Prefers
// the account's stored raw oauthAccount text (preserving every Claude Code
// profile field), falling back to a synthesized minimal object.
inline bool writeOauthAccount(const Vault& v, const Account& a) {
    std::string raw;
    std::string value = v.readAcctJson(a.num, raw) ? raw : synthOauthAccount(a);
    std::wstring path = globalConfigPath();
    std::string cfg;
    if (!readFile(path, cfg) || cfg.empty()) cfg = "{}";
    if (!spliceTopLevelKey(cfg, "oauthAccount", value)) return false;
    return atomicWriteFile(path, cfg);
}

// Learn the live account's full oauthAccount object into the vault.
//
// Claude Code enriches ~/.claude.json's oauthAccount with profile fields
// (displayName, billingType, roles, trial dates) that only exist once an
// account has actually been live. An account added through the browser login
// therefore has no rich copy yet, and the first switch to it can only
// synthesize a minimal object. Calling this whenever the live identity is a
// managed account makes the vault self-healing: after an account has been
// active once, every later switch restores its profile verbatim.
//
// Writes only when the text actually changed, so it can be called on every
// refresh tick without churning the disk. Returns true if something was saved.
inline bool captureLiveProfile(Vault& v, const LiveState& live) {
    if (!live.haveIdentity || live.rawOauthAccount.empty()) return false;
    Account* acc = v.byEmail(live.email);
    if (!acc) return false;
    std::string stored;
    if (v.readAcctJson(acc->num, stored) && stored == live.rawOauthAccount) return false;
    return v.writeAcctJson(acc->num, live.rawOauthAccount);
}

// Capture the current live login into the vault (add or update-in-place).
// Identity comes from ~/.claude.json's oauthAccount. Returns the account
// number, or 0 on failure with `err` set.
inline int captureLive(Vault& v, std::string& err) {
    VaultLock lock;
    v.load();
    LiveState live = readLiveState();
    if (!live.haveCred) { err = "no live credential file (log in with Claude Code first)"; return 0; }
    if (!live.haveIdentity) { err = "no oauthAccount identity in ~/.claude.json"; return 0; }
    OAuthCred cred = parseCredentials(live.credJson);
    if (!cred.valid) { err = "live credential is not an OAuth credential"; return 0; }

    Account* existing = v.byEmail(live.email);
    Account acc;
    if (existing) acc = *existing;
    else acc.num = v.nextNum();
    acc.email = live.email;
    acc.accountUuid = live.accountUuid;
    acc.orgUuid = live.orgUuid;
    acc.orgName = live.orgName;

    if (!v.writeCred(acc.num, live.credJson)) { err = "DPAPI write failed"; return 0; }
    if (!live.rawOauthAccount.empty())
        v.writeAcctJson(acc.num, live.rawOauthAccount); // preserve full profile
    if (existing) *existing = acc;
    else v.accounts.push_back(acc);
    if (!v.save()) { err = "vault metadata write failed"; return 0; }
    return acc.num;
}

// Switch the active Claude Code login to account `num`.
inline bool switchTo(Vault& v, int num, std::string& err) {
    VaultLock lock;
    v.load();
    Account* target = v.byNum(num);
    if (!target) { err = "no such account"; return false; }
    std::string targetCred;
    if (!v.readCred(num, targetCred)) { err = "cannot read stored credential (DPAPI)"; return false; }

    // Back up the live credential (and its current profile text) into its own
    // slot first, so token rotations and any profile changes Claude Code made
    // since the last switch aren't lost.
    LiveState live = readLiveState();
    if (live.haveCred && live.haveIdentity) {
        if (Account* cur = v.byEmail(live.email)) {
            OAuthCred c = parseCredentials(live.credJson);
            if (c.valid) v.writeCred(cur->num, live.credJson); // best effort
            if (!live.rawOauthAccount.empty())
                v.writeAcctJson(cur->num, live.rawOauthAccount);
        }
    }

    if (!atomicWriteFile(credentialsPath(), targetCred)) {
        err = "failed to write credentials file";
        return false;
    }
    // Only rewrite the identity when it actually changes. If the live config
    // already names this account, leaving oauthAccount untouched preserves the
    // rich profile Claude Code keeps there (displayName, billingType, roles),
    // which a synthesized object would drop.
    if (live.haveIdentity && live.email == target->email) return true;
    if (!writeOauthAccount(v, *target)) {
        err = "credentials written, but updating ~/.claude.json failed";
        return false;
    }
    return true;
}

// Refresh a stored account's token via the OAuth endpoint and persist it.
inline RefreshError refreshStored(Vault& v, int num, std::string& err) {
    std::string credJson;
    if (!v.readCred(num, credJson)) { err = "cannot read stored credential"; return RefreshError::Transient; }
    OAuthCred c = parseCredentials(credJson);
    if (!c.valid) { err = "stored credential is not OAuth"; return RefreshError::NoRefreshToken; }
    RefreshError re = refreshCredentials(c);
    if (re == RefreshError::None) {
        if (!v.writeCred(num, serializeCredentials(c))) {
            err = "refreshed but DPAPI write failed";
            return RefreshError::Transient;
        }
    }
    return re;
}

} // namespace cswap
