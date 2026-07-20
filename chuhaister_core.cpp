// cswap-core — console front-end for the native core (Stage 1 test harness).
//
// Commands:
//   status            live login identity + credential kind/expiry
//   add-current       capture the live Claude Code login into the vault
//   list              vault accounts (the live one is marked)
//   usage [num]       limits for a stored account (default: live token)
//   switch <num>      make a stored account the active login
//   refresh <num>     force an OAuth token refresh for a stored account
//   remove <num>      delete an account from the vault
//
// Build: see build.ps1 (cswap-core.exe, console subsystem).

#include "core/oauth_login.h"
#include "core/switcher.h"

#include <conio.h>
#include <cstdio>
#include <string>

using namespace cswap;

static void printUsageWindows(const Usage& u) {
    if (u.fiveHour.present)
        printf("  5-hour:  %5.1f%%  %s\n", u.fiveHour.pct,
               u.fiveHour.resetsAt.empty() ? "" : ("resets " + u.fiveHour.resetsAt).c_str());
    if (u.sevenDay.present)
        printf("  7-day:   %5.1f%%  %s\n", u.sevenDay.pct,
               u.sevenDay.resetsAt.empty() ? "" : ("resets " + u.sevenDay.resetsAt).c_str());
    for (const ScopedWindow& s : u.scoped)
        printf("  %-8s %5.1f%%  %s\n", (s.name + ":").c_str(), s.pct,
               s.resetsAt.empty() ? "" : ("resets " + s.resetsAt).c_str());
    if (!u.fiveHour.present && !u.sevenDay.present && u.scoped.empty())
        printf("  (no usage windows in response)\n");
}

static int cmdStatus() {
    LiveState s = readLiveState();
    printf("config:      %s\n", narrow(globalConfigPath()).c_str());
    printf("credentials: %s (%s)\n", narrow(credentialsPath()).c_str(),
           s.haveCred ? "present" : "MISSING");
    if (s.haveIdentity) {
        printf("identity:    %s\n", s.email.c_str());
        if (!s.orgName.empty()) printf("org:         %s\n", s.orgName.c_str());
    } else {
        printf("identity:    (none in ~/.claude.json)\n");
    }
    if (s.haveCred) {
        OAuthCred c = parseCredentials(s.credJson);
        if (c.valid) {
            printf("credential:  OAuth, %s refresh token, %s\n",
                   c.refreshToken.empty() ? "NO" : "has",
                   tokenExpired(c) ? "EXPIRED/expiring" : "fresh");
        } else {
            printf("credential:  not an OAuth payload\n");
        }
    }
    return 0;
}

static int cmdAddCurrent(Vault& v) {
    std::string err;
    int num = captureLive(v, err);
    if (!num) {
        printf("error: %s\n", err.c_str());
        return 1;
    }
    printf("captured live login into account %d (%s)\n", num, v.byNum(num)->email.c_str());
    return 0;
}

static int cmdList(Vault& v) {
    LiveState live = readLiveState();
    captureLiveProfile(v, live);     // opportunistic; see switcher.h
    captureLiveCredential(v, live);  // keep the active account's token current
    if (v.accounts.empty()) {
        printf("no managed accounts (run: cswap-core add-current)\n");
        return 0;
    }
    for (const Account& a : v.accounts) {
        bool active = live.haveIdentity && live.email == a.email;
        printf("%c %d  %-30s %s%s\n", active ? '*' : ' ', a.num, a.email.c_str(),
               a.orgName.empty() ? "" : a.orgName.c_str(),
               a.disabled ? "  (disabled)" : "");
    }
    return 0;
}

static int cmdUsage(Vault& v, int num) {
    std::string credJson;
    std::string label;
    if (num == 0) {
        LiveState s = readLiveState();
        if (!s.haveCred) { printf("error: no live credential\n"); return 1; }
        credJson = s.credJson;
        label = s.haveIdentity ? s.email : "(live)";
    } else {
        if (!v.readCred(num, credJson)) { printf("error: cannot read account %d\n", num); return 1; }
        Account* a = v.byNum(num);
        label = a ? a->email : std::to_string(num);
    }
    OAuthCred c = parseCredentials(credJson);
    if (!c.valid) { printf("error: not an OAuth credential\n"); return 1; }
    if (tokenExpired(c) && num != 0) {
        // Stored inactive account: refresh first (never refresh the live
        // token here — Claude Code owns its rotation).
        printf("token expired, refreshing…\n");
        RefreshError re = refreshCredentials(c);
        if (re != RefreshError::None) { printf("error: refresh failed\n"); return 1; }
        v.writeCred(num, serializeCredentials(c));
    }
    Usage u = fetchUsage(c.accessToken);
    if (!u.ok) { printf("error: usage fetch failed (http %lu)\n", u.httpStatus); return 1; }
    printf("usage for %s:\n", label.c_str());
    printUsageWindows(u);
    return 0;
}

static int cmdSwitch(Vault& v, int num) {
    std::string err;
    if (!switchTo(v, num, err)) {
        printf("error: %s\n", err.c_str());
        return 1;
    }
    Account* a = v.byNum(num);
    printf("switched active login to account %d (%s)\n", num, a ? a->email.c_str() : "?");
    printf("restart Claude Code sessions to pick it up immediately\n");
    return 0;
}

static int cmdRefresh(Vault& v, int num) {
    std::string err;
    RefreshError re = refreshStored(v, num, err);
    switch (re) {
        case RefreshError::None: printf("token refreshed and stored\n"); return 0;
        case RefreshError::NoRefreshToken: printf("error: no refresh token (%s)\n", err.c_str()); return 1;
        case RefreshError::InvalidGrant: printf("error: refresh token rejected — re-login needed\n"); return 1;
        default: printf("error: transient failure (%s)\n", err.c_str()); return 1;
    }
}

static int cmdAddLogin(Vault& v) {
    // Loopback redirect so the browser returns the code to us directly; the
    // clipboard/paste paths remain as fallbacks.
    Loopback lb;
    if (!loopbackStart(lb)) { printf("error: could not open a local callback port\n"); return 1; }
    std::string redirect = loopbackRedirectUri(lb);

    Pkce pk = makePkce();
    std::string url = authorizeUrl(pk, redirect);
    printf("Opening your browser to log in…\n");
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    printf("\nIf the browser didn't open, paste this URL:\n%s\n\n", url.c_str());
    printf("Sign in and press Authorize — I'll pick the code up automatically.\n");
    printf("(If a page shows the code instead, click \"Copy code\" or paste it here.)\n> ");
    fflush(stdout);

    std::string code, cbState;
    char buf[4096] = {0};
    for (int i = 0; i < 300 * 4 && code.empty(); ++i) {
        if (loopbackPoll(lb, code, cbState, 250) && !code.empty()) break;
        if (clipboardHasOurCode(pk.state, code)) break;
        if (_kbhit() && fgets(buf, sizeof(buf), stdin)) { code = buf; break; }
    }
    loopbackStop(lb);
    if (code.empty()) { printf("\nerror: timed out waiting for the authorization code\n"); return 1; }
    printf("\ngot the code, exchanging for tokens…\n");

    OAuthCred cred;
    std::string err;
    if (!exchangeCode(code, pk, redirect, cred, err)) {
        printf("error: %s\n", err.c_str());
        return 1;
    }
    int num = storeLogin(v, cred, err);
    if (!num) { printf("error: %s\n", err.c_str()); return 1; }
    printf("added account %d (%s)\n", num, v.byNum(num)->email.c_str());
    return 0;
}

static int cmdRemove(Vault& v, int num) {
    if (!v.byNum(num)) { printf("error: no such account\n"); return 1; }
    v.removeAccount(num);
    if (!v.save()) { printf("error: vault write failed\n"); return 1; }
    printf("removed account %d\n", num);
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Vault v;
    if (!v.load()) {
        printf("error: vault metadata is corrupt (%s)\n", narrow(v.accountsPath()).c_str());
        return 1;
    }
    std::string cmd = argc > 1 ? argv[1] : "list";
    int num = argc > 2 ? atoi(argv[2]) : 0;

    if (cmd == "status") return cmdStatus();
    if (cmd == "add-current") return cmdAddCurrent(v);
    if (cmd == "add-login") return cmdAddLogin(v);
    if (cmd == "authorize-url") { printf("%s\n", authorizeUrl(makePkce()).c_str()); return 0; }
    if (cmd == "list") return cmdList(v);
    if (cmd == "usage") return cmdUsage(v, num);
    if (cmd == "switch") { if (!num) { printf("usage: cswap-core switch <num>\n"); return 1; } return cmdSwitch(v, num); }
    if (cmd == "refresh") { if (!num) { printf("usage: cswap-core refresh <num>\n"); return 1; } return cmdRefresh(v, num); }
    if (cmd == "remove") { if (!num) { printf("usage: cswap-core remove <num>\n"); return 1; } return cmdRemove(v, num); }
    printf("commands: status | add-current | add-login | list | usage [num] | switch <num> | refresh <num> | remove <num>\n");
    return 1;
}
