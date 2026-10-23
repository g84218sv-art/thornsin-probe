// thornsin_probe.cpp  --  READ-ONLY probe for ThornSin v0.7.5
//
// Purpose: confirm the pointer chain and field offsets that the old trainer
// (ThornSinCheat.exe by 0C72) used, BEFORE a new writing trainer is built.
// This program never writes to the game and never injects anything.
//
// Build (MinGW-w64):   g++ -O2 -std=c++17 -static -o thornsin_probe.exe thornsin_probe.cpp
// Build (MSVC):        cl /O2 /EHsc /std:c++17 thornsin_probe.cpp
//
// Run: start ThornSin first, get into gameplay, then run thornsin_probe.exe.
// A new block is printed every time any value changes. In the game, do one
// thing at a time (take damage, use mana, pick up gold ...) and note which
// row moved. Copy the printed text and send it back.

#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

typedef unsigned long long u64;

static const u64 RVA_ROOT = 0x20B0F50;   // GameAssembly.dll + this = chain root (taken from the old trainer)

struct Game {
    HANDLE h = nullptr;
    DWORD  pid = 0;
    u64    ga = 0;                        // GameAssembly.dll base
};

static bool rd(HANDLE h, u64 addr, void* out, size_t n) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(h, (LPCVOID)addr, out, n, &got)) return false;
    return got == n;
}

static bool plausible_ptr(u64 p) {
    return p >= 0x10000ULL && p < 0x00007FFFFFFF0000ULL && (p & 7) == 0;
}

static bool rd_ptr(HANDLE h, u64 addr, u64& out) {
    u64 v = 0;
    if (!rd(h, addr, &v, 8) || !plausible_ptr(v)) return false;
    out = v;
    return true;
}

static void close_game(Game& g) {
    if (g.h) CloseHandle(g.h);
    g = Game();
}

static bool find_game(Game& g) {
    DWORD pid = 0;
    {
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (s == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe; memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
        if (Process32FirstW(s, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"ThornSin.exe") == 0) { pid = pe.th32ProcessID; break; }
            } while (Process32NextW(s, &pe));
        }
        CloseHandle(s);
    }
    if (!pid) return false;

    u64 base = 0;
    {
        HANDLE m = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (m == INVALID_HANDLE_VALUE) return false;
        MODULEENTRY32W me; memset(&me, 0, sizeof(me)); me.dwSize = sizeof(me);
        if (Module32FirstW(m, &me)) {
            do {
                if (_wcsicmp(me.szModule, L"GameAssembly.dll") == 0) { base = (u64)me.modBaseAddr; break; }
            } while (Module32NextW(m, &me));
        }
        CloseHandle(m);
    }
    if (!base) return false;   // game still loading

    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    g.h = h; g.pid = pid; g.ga = base;
    return true;
}

struct Row {
    const char* name;
    u64  addr;
    bool ok;
    unsigned int raw;
};

struct Snap {
    u64 p1, p2, p3, g60, g68;
    std::vector<Row> rows;
};

struct FieldDef { const char* base; int off; };
static const FieldDef FIELDS[] = {
    // base "A" = p3 itself (what the old trainer called G58)
    {"A", 0x00}, {"A", 0x44}, {"A", 0x8C}, {"A", 0x90},
    // base "B" = [p3 + 0x48]  (old G60)
    {"B", 0x10}, {"B", 0x14}, {"B", 0x18}, {"B", 0x28}, {"B", 0x2C},
    // base "C" = [p3 + 0xD0]  (old G68)
    {"C", 0x00}, {"C", 0x3C}, {"C", 0x40}, {"C", 0x44}, {"C", 0x68}, {"C", 0x88},
};

static bool take_snapshot(const Game& g, Snap& s) {
    s = Snap();
    if (!rd_ptr(g.h, g.ga + RVA_ROOT, s.p1)) return false;   // class pointer
    if (!rd_ptr(g.h, s.p1 + 0xB8, s.p2))     return false;   // static fields
    if (!rd_ptr(g.h, s.p2 + 0x38, s.p3))     return false;   // instance (old G58)
    if (!rd_ptr(g.h, s.p3 + 0x48, s.g60))    return false;   // old G60
    if (!rd_ptr(g.h, s.p3 + 0xD0, s.g68))    return false;   // old G68

    for (const FieldDef& f : FIELDS) {
        u64 b = (f.base[0] == 'A') ? s.p3 : (f.base[0] == 'B') ? s.g60 : s.g68;
        Row r; r.name = f.base; r.addr = b + (u64)f.off; r.ok = false; r.raw = 0;
        char* label = new char[16];
        snprintf(label, 16, "%s+0x%02X", f.base, f.off);
        r.name = label;
        r.ok = rd(g.h, r.addr, &r.raw, 4);
        s.rows.push_back(r);
    }
    return true;
}

static bool same(const Snap& a, const Snap& b) {
    if (a.p3 != b.p3 || a.g60 != b.g60 || a.g68 != b.g68) return false;
    if (a.rows.size() != b.rows.size()) return false;
    for (size_t i = 0; i < a.rows.size(); i++)
        if (a.rows[i].ok != b.rows[i].ok || a.rows[i].raw != b.rows[i].raw) return false;
    return true;
}

static void print_snap(const Snap& s, const Game& g) {
    SYSTEMTIME t; GetLocalTime(&t);
    printf("---- %02d:%02d:%02d ----\n", t.wHour, t.wMinute, t.wSecond);
    printf("GameAssembly=0x%llx  root=[GA+0x%llX]\n", (u64)g.ga, (u64)RVA_ROOT);
    printf("p1=0x%llx  p2=0x%llx  p3(A)=0x%llx  B=0x%llx  C=0x%llx\n",
           (u64)s.p1, (u64)s.p2, (u64)s.p3, (u64)s.g60, (u64)s.g68);
    for (const Row& r : s.rows) {
        if (!r.ok) { printf("  %-8s  <unreadable>\n", r.name); continue; }
        float f; memcpy(&f, &r.raw, 4);
        printf("  %-8s  float=%-14.4f int=%-12d hex=0x%08X\n", r.name, (double)f, (int)r.raw, r.raw);
    }
    fflush(stdout);
}

int main() {
    printf("ThornSin probe (READ-ONLY). Waiting for ThornSin.exe ...\n");
    Game g;
    Snap last; bool have_last = false; bool warned_wait = false;

    for (;;) {
        if (g.h && WaitForSingleObject(g.h, 0) == WAIT_OBJECT_0) {
            printf("Game closed.\n");
            close_game(g); have_last = false; warned_wait = false;
        }
        if (!g.h) {
            if (!find_game(g)) { Sleep(1000); continue; }
            printf("Attached: pid=%lu\n", (unsigned long)g.pid);
        }

        Snap s;
        if (!take_snapshot(g, s)) {
            if (!warned_wait) {
                printf("Pointer chain not valid yet (menu / loading?). Waiting ...\n");
                warned_wait = true;
            }
            have_last = false;
            Sleep(500);
            continue;
        }
        warned_wait = false;

        if (!have_last || !same(s, last)) {
            print_snap(s, g);
            last = s; have_last = true;
        }
        Sleep(250);
    }
}
