// Account vault for the cswap native core.
//
// Layout under %USERPROFILE%\.cswap\ :
//   accounts.json   — metadata only: [{num, email, orgName, orgUuid, accountUuid}]
//   cred-<num>.bin  — that account's credentials JSON, DPAPI-protected
//                     (current-user scope; unreadable from other Windows users)
//   acct-<num>.json — the account's full oauthAccount object, raw text as it
//                     appeared in ~/.claude.json (Claude Code stores rich
//                     profile fields there — displayName, billingType, roles —
//                     which a switch must restore verbatim, not synthesize)
//
// The vault never holds plaintext tokens on disk, and metadata carries no
// secrets — safe to read for list/menu rendering without touching DPAPI.

#pragma once

#include "json.h"
#include "win_util.h"

#include <string>
#include <vector>

namespace cswap {

// Cross-process guard for the vault's read-modify-write cycles. Adding,
// switching and removing all do load → change → save, and the tray and the CLI
// can run them at the same time; without this, one save silently drops the
// other's account (observed: an account vanishing while a concurrent add ran).
class VaultLock {
public:
    explicit VaultLock(DWORD waitMs = 15000) {
        h_ = CreateMutexW(nullptr, FALSE, L"Local\\cswap-vault-lock");
        if (h_) WaitForSingleObject(h_, waitMs);
    }
    ~VaultLock() {
        if (h_) { ReleaseMutex(h_); CloseHandle(h_); }
    }
    VaultLock(const VaultLock&) = delete;
    VaultLock& operator=(const VaultLock&) = delete;

private:
    HANDLE h_ = nullptr;
};

struct Account {
    int num = 0;
    std::string email;
    std::string accountUuid;
    std::string orgUuid;
    std::string orgName;
    bool disabled = false;
};

struct Vault {
    std::wstring dir = vaultDir();
    std::vector<Account> accounts;

    std::wstring accountsPath() const { return dir + L"\\accounts.json"; }
    std::wstring credPath(int num) const {
        return dir + L"\\cred-" + std::to_wstring(num) + L".bin";
    }
    std::wstring acctJsonPath(int num) const {
        return dir + L"\\acct-" + std::to_wstring(num) + L".json";
    }

    bool writeAcctJson(int num, const std::string& rawOauthAccount) const {
        return ensureDir(dir) && atomicWriteFile(acctJsonPath(num), rawOauthAccount);
    }

    bool readAcctJson(int num, std::string& rawOut) const {
        return readFile(acctJsonPath(num), rawOut) && !rawOut.empty();
    }

    bool load() {
        accounts.clear();
        std::string text;
        if (!readFile(accountsPath(), text)) return true; // empty vault is fine
        JParser p(text);
        JVal root = p.parse();
        const JVal* arr = root.get("accounts");
        if (!p.ok || !arr || arr->type != JVal::ARR) return false;
        for (const JVal& a : arr->arr) {
            if (a.type != JVal::OBJ) continue;
            Account acc;
            acc.num = (int)(a.get("num") ? a.get("num")->numOr(0) : 0);
            acc.email = a.get("email") ? a.get("email")->strOr("") : "";
            acc.accountUuid = a.get("accountUuid") ? a.get("accountUuid")->strOr("") : "";
            acc.orgUuid = a.get("orgUuid") ? a.get("orgUuid")->strOr("") : "";
            acc.orgName = a.get("orgName") ? a.get("orgName")->strOr("") : "";
            acc.disabled = a.get("disabled") && a.get("disabled")->boolOr(false);
            if (acc.num > 0 && !acc.email.empty()) accounts.push_back(acc);
        }
        return true;
    }

    bool save() const {
        std::string out = "{\n  \"version\": 1,\n  \"accounts\": [";
        for (size_t i = 0; i < accounts.size(); ++i) {
            const Account& a = accounts[i];
            out += std::string(i ? "," : "") + "\n    {\"num\": " + std::to_string(a.num)
                 + ", \"email\": " + jsonString(a.email)
                 + ", \"accountUuid\": " + jsonString(a.accountUuid)
                 + ", \"orgUuid\": " + jsonString(a.orgUuid)
                 + ", \"orgName\": " + jsonString(a.orgName)
                 + (a.disabled ? ", \"disabled\": true" : "") + "}";
        }
        out += "\n  ]\n}\n";
        return ensureDir(dir) && atomicWriteFile(accountsPath(), out);
    }

    Account* byNum(int num) {
        for (Account& a : accounts)
            if (a.num == num) return &a;
        return nullptr;
    }

    Account* byEmail(const std::string& email) {
        for (Account& a : accounts)
            if (a.email == email) return &a;
        return nullptr;
    }

    int nextNum() const {
        int n = 1;
        for (const Account& a : accounts)
            if (a.num >= n) n = a.num + 1;
        return n;
    }

    bool writeCred(int num, const std::string& credJson) const {
        std::string blob;
        if (!dpapiProtect(credJson, blob)) return false;
        return ensureDir(dir) && atomicWriteFile(credPath(num), blob);
    }

    bool readCred(int num, std::string& credJson) const {
        std::string blob;
        if (!readFile(credPath(num), blob)) return false;
        return dpapiUnprotect(blob, credJson);
    }

    void removeAccount(int num) {
        DeleteFileW(credPath(num).c_str());
        DeleteFileW(acctJsonPath(num).c_str());
        for (size_t i = 0; i < accounts.size(); ++i)
            if (accounts[i].num == num) {
                accounts.erase(accounts.begin() + i);
                break;
            }
    }
};

} // namespace cswap
