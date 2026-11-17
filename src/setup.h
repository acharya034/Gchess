// =====================================================================================
//  setup.h  -  "board setup engine": tells the user what to move so the physical board
//              matches a TARGET position. Pure logic + LED hints, no display code.
//
//  Squares: a1 = 0 ... h8 = 63 (same as boardOccupancy()). Pieces: cr::Position b[] (+white / -black).
//
//  PARTS
//    1) last board memory  (NVS key "lastbrd"): lastBoardSave/Load/Invalidate/Valid/Flush/Tick
//    2) target sources     : setupTargetStart(), setupTargetFen()  (+ setupTargetFromGame() in game.h)
//    3) planner            : setupPlan()  STATELESS, same input -> same steps
//    4) engine             : setupBegin(), setupTick(occ), setupConfirm(), setupEnd(), getters
//    5) LEDs               : drawn only while the engine is active
//
//  LED BEHAVIOUR (setup)
//    - All LEDs are OFF by default (no white glow, no red "wrong piece" squares).
//    - A piece was just placed: that square glows GREEN (wanted square) or RED (not wanted)
//      for SETUP_FEEDBACK_MS. Wrong pieces that were already on the board never glow.
//    - The user pauses for SETUP_HINT_MS: only the NEXT step blinks
//        MOVE   : source blue, destination green
//        REMOVE : the square red
//        PLACE  : the square green (red on an unknown board)
//    - Unknown board: NO "remove everything" phase. One square at a time is guided.
//
//  Everything here runs in the UI task (flash writes!). NEVER call it from hwTask.
// =====================================================================================
#pragma once
#include "gchess_config.h"
#include "debug.h"
#include "chess_rules.h"
#include "storage.h"
#include "leds.h"
#include "board.h"

#ifndef SETUP_HINT_MS
#define SETUP_HINT_MS 1000      // idle time before the guide LED blinks
#endif
#ifndef SETUP_FEEDBACK_MS
#define SETUP_FEEDBACK_MS 500   // green / red flash after a piece is placed
#endif

// ---------- 1) last board memory ------------------------------------------------------
// What was physically on the board when we last knew for sure. Stored with a checksum.
struct LastBoard {
    uint32_t magic;       // LASTBRD_MAGIC (change it if the layout changes)
    uint32_t crc;         // CRC32 of everything after this field
    uint8_t valid;        // 1 = pieces[] describes the real board
    uint8_t rsv[7];
    uint64_t occ;         // occupancy mask (a1 = bit 0)
    int8_t pieces[64];    // cr::Position b[]
};
#define LASTBRD_MAGIC 0x3144424Cu   // "LBD1"

static LastBoard gLb;                 // RAM copy (always the newest known state)
static LastBoard gLbFlash;            // what is in flash right now (to skip identical writes)
static bool gLbDirty = false;         // RAM copy newer than flash
static uint32_t gLbLastWrite = 0;     // millis() of the last flash write
static uint32_t gLbChangedAt = 0;     // millis() of the last change of the RAM copy

// Small bitwise CRC32 (80 bytes, called rarely).
inline uint32_t lbCrc(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// Fill the checksum of a LastBoard.
inline void lbSeal(LastBoard& b) {
    b.magic = LASTBRD_MAGIC;
    b.crc = lbCrc((const uint8_t*)&b + 8, sizeof(LastBoard) - 8);
}

// Is a LastBoard intact and self consistent?
inline bool lbCheck(const LastBoard& b) {
    if (b.magic != LASTBRD_MAGIC) return false;
    if (b.crc != lbCrc((const uint8_t*)&b + 8, sizeof(LastBoard) - 8)) return false;
    uint64_t o = 0;
    for (int i = 0; i < 64; i++) if (b.pieces[i]) o |= 1ULL << i;
    return o == b.occ;
}

// Write the RAM copy to flash now (unless identical to what is already there).
inline void lastBoardFlush() {
    if (!gLbDirty) return;
    gLbDirty = false;
    gLbLastWrite = millis();
    if (!memcmp(&gLb, &gLbFlash, sizeof(LastBoard))) return;
    gPrefs.putBytes("lastbrd", &gLb, sizeof(gLb));
    gLbFlash = gLb;
    DBG(SETUP, "last board written to flash (valid=%d pieces=%d)", (int)gLb.valid, __builtin_popcountll(gLb.occ));
}

// Read the saved board from flash (call once in setup(), after storageBegin()).
inline void lastBoardLoad() {
    memset(&gLb, 0, sizeof(gLb));
    gLbDirty = false;
    if (gPrefs.isKey("lastbrd") && gPrefs.getBytesLength("lastbrd") == sizeof(LastBoard)) {
        gPrefs.getBytes("lastbrd", &gLb, sizeof(gLb));
        if (!lbCheck(gLb)) memset(&gLb, 0, sizeof(gLb));   // damaged -> forget
    }
    gLbFlash = gLb;
    DBG(SETUP, "last board: %s (pieces=%d)", gLb.valid ? "valid" : "none", __builtin_popcountll(gLb.occ));
}

inline bool lastBoardValid() {
    return gLb.valid && gLb.magic == LASTBRD_MAGIC;
}

// Remember a position as "what is on the board" (RAM only, no flash wear).
// Flash is written only by lastBoardFlush(), called from powerOff() / powerRestart().
// IMPORTANT: call this after EVERY game move too, otherwise the saved map gets stale
// and the planner can not know which piece went where.
inline void lastBoardSave(const cr::Position& p) {
    LastBoard n;
    memset(&n, 0, sizeof(n));
    n.valid = 1;
    memcpy(n.pieces, p.b, 64);
    n.occ = cr::occupancy(p);
    lbSeal(n);
    if (!memcmp(&n, &gLb, sizeof(n))) return;   // nothing new
    gLb = n;
    gLbDirty = true;
    gLbChangedAt = millis();                    // RAM only; flash is written by lastBoardFlush() at power off / restart
}

// Forget the saved board (we no longer trust it).
inline void lastBoardInvalidate() {
    if (!gLb.valid && !gLbFlash.valid) return;
    memset(&gLb, 0, sizeof(gLb));
    gLbDirty = true;
    gLbChangedAt = millis();                    // RAM only, flushed at power off / restart
    DBG(SETUP, "last board invalidated");
}

// UI task, every loop: write a dirty map after 20 s of quiet (at most once a minute), so a pulled battery or USB cable loses at most the last moves.
inline void lastBoardTick() {
    if (!gLbDirty) return;
    uint32_t now = millis();
    if (now - gLbChangedAt < 20000 || now - gLbLastWrite < 60000) return;
    DBG(SETUP, "last board: idle flush");
    lastBoardFlush();
}

// ---------- 2) target sources ---------------------------------------------------------
inline void setupTargetStart(cr::Position& out) {
    cr::setStart(out);
}

// Any FEN (puzzles). Returns false when the FEN is bad (out is then the start position).
inline bool setupTargetFen(cr::Position& out, const char* fen) {
    if (fen && fen[0] && strcmp(fen, "startpos") && cr::fromFen(out, fen)) return true;
    cr::setStart(out);
    return false;
}

// ---------- 3) planner (stateless) -----------------------------------------------------
enum StepKind : uint8_t { ST_NONE, ST_MOVE, ST_REMOVE, ST_PLACE };
struct SetupStep {
    uint8_t kind;    // StepKind
    uint8_t sq;      // MOVE: from   REMOVE: square   PLACE: square
    uint8_t to;      // MOVE: destination
    uint8_t type;    // piece type 1..6 (cr::P..cr::K); 0 for REMOVE of an unknown piece
    bool white;      // piece colour (MOVE / PLACE)
};

// Where to start with "friendly order" PLACE steps: the side the user plays, from the user's edge.
inline int suOrderKey(const cr::Position& t, int sq, bool blackSide) {
    int8_t pc = t.b[sq];
    bool pcBlack = pc < 0;
    int side = (pcBlack == blackSide) ? 0 : 1;          // own colour first
    int pawn = (abs(pc) == cr::P) ? 1 : 0;              // back rank pieces before pawns
    int pos = blackSide ? (63 - sq) : sq;               // from the user's edge, left to right
    return side * 1000 + pawn * 100 + pos;
}

// Squared distance between two squares.
inline int suDist2(int a, int b) {
    int df = (a & 7) - (b & 7), dr = (a >> 3) - (b >> 3);
    return df * df + dr * dr;
}

// Build the ordered step list.
//   occ     = occupancy right now
//   assumed = saved piece map (what we believed was on the board), or nullptr = unknown board
// Idea: first rebuild "which piece is probably on which square now" (cur[]), then
//   1) pair every wrong piece with a target square that needs the SAME piece (nearest first)
//   2) order those MOVEs so the destination is EMPTY first  (B->A before C->B)
//   3) leftover wrong pieces -> REMOVE, leftover needed squares -> PLACE
// out holds at most max steps; *total = real number of steps (may be bigger than max).
inline uint8_t setupPlan(uint64_t occ, const int8_t* assumed, const cr::Position& tgt, bool blackSide,
                         SetupStep* out, uint8_t max, uint8_t* total) {
    uint64_t want = cr::occupancy(tgt);
    uint8_t n = 0;
    int tot = 0;
    int8_t cur[64];                       // best guess of the piece on each square (99 = some unknown piece)
    memset(cur, 0, sizeof(cur));

    if (!assumed) {
        // Unknown board: only occupancy is known. A piece on a wanted square is assumed right,
        // a piece on an unwanted square is "some piece" that has to go.
        for (int i = 0; i < 64; i++)
            if ((occ >> i) & 1) cur[i] = ((want >> i) & 1) ? tgt.b[i] : 99;
    } else {
        uint64_t aocc = 0;
        for (int i = 0; i < 64; i++) if (assumed[i]) aocc |= 1ULL << i;
        uint64_t lifted = aocc & ~occ;    // squares that lost their piece
        uint64_t arrived = occ & ~aocc;   // squares that got a piece
        for (uint64_t k = aocc & occ; k; k &= k - 1) {
            int i = __builtin_ctzll(k);
            cur[i] = assumed[i];          // untouched squares
        }
        // a piece that appeared somewhere = a piece that was lifted somewhere
        while (arrived && lifted) {
            int ba = -1, bl = -1, bs = 1 << 30;
            for (uint64_t a = arrived; a; a &= a - 1) {
                int ia = __builtin_ctzll(a);
                for (uint64_t l = lifted; l; l &= l - 1) {
                    int il = __builtin_ctzll(l);
                    int sc = (assumed[il] == tgt.b[ia] ? 0 : 1000) + suDist2(il, ia);
                    if (sc < bs) { bs = sc; ba = ia; bl = il; }
                }
            }
            cur[ba] = assumed[bl];
            arrived &= ~(1ULL << ba);
            lifted &= ~(1ULL << bl);
        }
        for (uint64_t a = arrived; a; a &= a - 1) {   // new piece from nowhere (user added one)
            int ia = __builtin_ctzll(a);
            cur[ia] = ((want >> ia) & 1) ? tgt.b[ia] : 99;
        }
    }

    uint64_t src = 0;                     // a wrong piece stands here
    uint64_t dst = 0;                     // a piece is needed here
    for (int i = 0; i < 64; i++) {
        if (cur[i] == tgt.b[i]) continue;
        if (cur[i]) src |= 1ULL << i;
        if (tgt.b[i]) dst |= 1ULL << i;
    }

    // 1) pair wrong pieces with squares that need the SAME piece, nearest first
    uint8_t mvS[32], mvD[32];
    int nm = 0;
    while (src && dst && nm < 32) {
        int bs = -1, bd = -1, bdist = 1 << 30;
        for (uint64_t s = src; s; s &= s - 1) {
            int a = __builtin_ctzll(s);
            for (uint64_t m = dst; m; m &= m - 1) {
                int b = __builtin_ctzll(m);
                if (cur[a] != tgt.b[b]) continue;
                int d = suDist2(a, b);
                if (d < bdist) { bdist = d; bs = a; bd = b; }   // ties: lowest squares (deterministic)
            }
        }
        if (bs < 0) break;
        mvS[nm] = (uint8_t)bs;
        mvD[nm] = (uint8_t)bd;
        nm++;
        src &= ~(1ULL << bs);
        dst &= ~(1ULL << bd);
    }

    // 2) order the moves: a move whose destination is empty goes first
    uint64_t sim = occ;
    bool used[32] = {};
    for (int k = 0; k < nm; k++) {
        int pick = -1, pd = 1 << 30;
        bool pickFree = false;
        for (int i = 0; i < nm; i++) {
            if (used[i]) continue;
            bool fr = !((sim >> mvD[i]) & 1);
            int d = suDist2(mvS[i], mvD[i]);
            if (pick < 0 || (fr && !pickFree) || (fr == pickFree && d < pd)) { pick = i; pd = d; pickFree = fr; }
        }
        used[pick] = true;
        int8_t pc = tgt.b[mvD[pick]];
        if (n < max) out[n++] = SetupStep{ST_MOVE, mvS[pick], mvD[pick], (uint8_t)abs(pc), pc > 0};
        tot++;
        sim &= ~(1ULL << mvS[pick]);
        sim |= 1ULL << mvD[pick];
    }

    // 3) leftover wrong pieces -> REMOVE
    for (uint64_t s = src; s; s &= s - 1) {
        int a = __builtin_ctzll(s);
        bool known = cur[a] != 99;
        if (n < max) out[n++] = SetupStep{ST_REMOVE, (uint8_t)a, 0, (uint8_t)(known ? abs(cur[a]) : 0), known ? cur[a] > 0 : true};
        tot++;
    }

    // 4) leftover needed squares -> PLACE in friendly order (selection by key, no sorting buffers)
    int lastKey = -1;
    for (uint64_t m = dst; m; m &= m - 1) tot++;
    for (;;) {
        int best = -1, bk = 1 << 30;
        for (uint64_t m = dst; m; m &= m - 1) {
            int b = __builtin_ctzll(m);
            int k = suOrderKey(tgt, b, blackSide);
            if (k > lastKey && k < bk) { bk = k; best = b; }
        }
        if (best < 0) break;
        lastKey = bk;
        if (n < max) out[n++] = SetupStep{ST_PLACE, (uint8_t)best, 0, (uint8_t)abs(tgt.b[best]), tgt.b[best] > 0};
    }
    *total = (uint8_t)(tot > 255 ? 255 : tot);
    return n;
}

// ---------- 4) engine ------------------------------------------------------------------
// S_CLEAR_ALL is kept in the enum so other files still compile, but it is never entered any more.
enum SetupState : uint8_t { S_IDLE, S_CLEAR_ALL, S_RESTORE, S_PLACE, S_VERIFY, S_DONE };
enum SetupMode : uint8_t { SM_WHITE, SM_BLACK };      // which side the user plays (placement order)
enum BootCheck : uint8_t { BOOT_MATCHES_SAVED, BOOT_RESTORABLE, BOOT_UNKNOWN };
static BootCheck gBootCheck = BOOT_UNKNOWN;   // result of the boot check (valid when gBootChecked)
static bool gBootChecked = false;             // sensors were stable and the check ran
static bool gBootUsed = false;                // the first game start already looked at it

struct SetupEngine {
    SetupState st = S_IDLE;
    cr::Position tgt;
    uint64_t want = 0;
    int8_t assumed[64];           // copy of the saved piece map taken at setupBegin()
    bool black = false;           // user plays black
    bool unknown = false;         // latched at begin: saved map not usable
    bool cleared = false;         // unused now (kept for compatibility)
    bool confirmed = false;       // user said "yes, it is right"
    bool planned = false;
    uint64_t occ = 0;             // occupancy of the last plan
    uint64_t bad = 0;             // pieces where nothing is wanted (not drawn any more, only for logs/getters)
    SetupStep steps[SETUP_MAX_STEPS];
    uint8_t nSteps = 0, total = 0;
    bool matching = false;
    uint32_t matchFrame0 = 0;
    bool stable = false;
    uint32_t drawSig = 0;
    uint32_t drawAt = 0;
    // --- placement feedback + idle hint ---
    uint64_t prevOcc = 0;         // occupancy at the previous tick (to see what was just placed)
    uint32_t lastChange = 0;      // millis() of the last occupancy change
    uint64_t fbGood = 0;          // squares just filled that are wanted   -> green
    uint64_t fbBad = 0;           // squares just filled that are NOT wanted -> red
    uint32_t fbUntil = 0;         // feedback visible until this millis() (0 = none)
    bool hintOn = false;          // the guide LED is blinking
};
static SetupEngine gSu;

inline bool setupActive() {
    return gSu.st != S_IDLE;
}

inline const char* setupStateName(SetupState s) {
    static const char* n[] = {"IDLE", "CLEAR_ALL", "RESTORE", "PLACE", "VERIFY", "DONE"};
    return n[s];
}

// How does the board on the table compare with the saved one? (call at boot or before setupBegin)
inline BootCheck setupBootCheck(uint64_t occ) {
    if (!lastBoardValid()) return BOOT_UNKNOWN;
    if (occ == gLb.occ) return BOOT_MATCHES_SAVED;
    if (__builtin_popcountll(occ ^ gLb.occ) <= SETUP_MAX_RESTORE_DIFF) return BOOT_RESTORABLE;
    return BOOT_UNKNOWN;
}

// "M e4>e2 R-d3 P+Ng1" - one line for the log instead of 30.
inline void suStepsToStr(char* o, size_t n) {
    size_t k = 0;
    o[0] = 0;
    for (int i = 0; i < gSu.nSteps && k + 12 < n; i++) {
        const SetupStep& s = gSu.steps[i];
        char a[3], b[3];
        cr::sqName(s.sq, a);
        cr::sqName(s.to, b);
        char pc = s.type ? "?PNBRQK"[s.type] : '?';
        if (!s.white && s.type) pc = pc + 32;
        if (s.kind == ST_MOVE) k += snprintf(o + k, n - k, "M%c%s>%s ", pc, a, b);
        else if (s.kind == ST_REMOVE) k += snprintf(o + k, n - k, "R-%s ", a);
        else k += snprintf(o + k, n - k, "P+%c%s ", pc, a);
    }
    if (gSu.nSteps < gSu.total && k + 4 < n) strcat(o, "...");
}

// Squares with a piece where the target is empty and that are NOT part of the current step.
inline uint64_t setupBadMask() {
    return gSu.bad;
}

inline const SetupStep* setupCurrentStep() {
    return gSu.nSteps ? &gSu.steps[0] : nullptr;
}

inline int setupRemaining() {
    return gSu.total;
}

// True when occ == target occupancy, stable for SENSOR_DEBOUNCE frames.
inline bool setupDone() {
    return gSu.stable;
}

// Run the planner again for this occupancy (cheap, no state is kept except the begin-time latches).
inline void setupReplan(uint64_t occ) {
    gSu.occ = occ;
    gSu.planned = true;
    gSu.nSteps = setupPlan(occ, gSu.unknown ? nullptr : gSu.assumed, gSu.tgt, gSu.black, gSu.steps, SETUP_MAX_STEPS, &gSu.total);

    uint64_t cur = 0;
    if (gSu.nSteps) {
        cur = 1ULL << gSu.steps[0].sq;
        if (gSu.steps[0].kind == ST_MOVE) cur |= 1ULL << gSu.steps[0].to;
    }
    uint64_t oldBad = gSu.bad;
    gSu.bad = occ & ~gSu.want & ~cur;
    char buf[200];
    suStepsToStr(buf, sizeof(buf));
    DBG(SETUP, "plan: %d step(s) of %d: %s", (int)gSu.nSteps, (int)gSu.total, buf);
    if (gSu.bad != oldBad && gSu.bad) {
        char sq[48];
        dbgSquares(gSu.bad, sq, sizeof(sq));
        DBG(SETUP, "bad squares [%s]", sq);
    }
}

// ---------- 5) LEDs (only while the engine is active) ------------------------------------
#define SU_BLUE   CRGB(0, 70, 255)
#define SU_GREEN  CRGB(0, 255, 60)
#define SU_RED    CRGB(255, 20, 0)
#define SU_DIM    CRGB(35, 35, 35)   // no longer used in setup.h; kept because pages.h still references it

// Dark by default. Draws (a) the short green/red answer to a piece that was just placed and
// (b) after a pause, a blink on the next step only. Never shows pieces that were already wrong.
inline void setupDrawLeds() {
    if (gSu.st == S_IDLE) return;                     // never touch a running game's LEDs
    ledsClear();
    ledsClearOverlay();
    uint32_t now = millis();

    // (a) feedback for a piece that was JUST placed
    if (gSu.fbUntil && (int32_t)(gSu.fbUntil - now) > 0) {
        for (uint64_t m = gSu.fbGood; m; m &= m - 1) ledSq(__builtin_ctzll(m), SU_GREEN);
        for (uint64_t m = gSu.fbBad; m; m &= m - 1) ledSq(__builtin_ctzll(m), SU_RED);
    }

    // (b) guide: only the current step, only after the user paused
    const SetupStep* s = setupCurrentStep();
    if (gSu.hintOn && s && (gSu.st == S_PLACE || gSu.st == S_RESTORE)) {
        if (s->kind == ST_MOVE) {
            ledsFlashMask(1ULL << s->sq, SU_BLUE, 60000);     // pick up here
            ledsFlashMask(1ULL << s->to, SU_GREEN, 60000);    // put down here
        } else if (s->kind == ST_REMOVE) {
            ledsFlashMask(1ULL << s->sq, SU_RED, 60000);      // pick up (remove / check) this one
        } else if (s->kind == ST_PLACE) {
            ledsFlashMask(1ULL << s->sq, gSu.unknown ? SU_RED : SU_GREEN, 60000);
        }
    }
    // S_VERIFY / S_DONE: nothing is drawn
}

// Work out the state from (occupancy, latches). Nothing is counted: same input -> same state.
inline SetupState setupDerive() {
    if (gSu.stable) return gSu.confirmed ? S_DONE : S_VERIFY;
    if (gSu.unknown) return S_PLACE;                  // no clear-all phase any more
    for (int i = 0; i < gSu.nSteps; i++) if (gSu.steps[i].kind != ST_PLACE) return S_RESTORE;
    return S_PLACE;
}

// Start a setup. forceUnknown = ignore the saved board (e.g. the user said it was wrong).
inline void setupBegin(const cr::Position& target, SetupMode mode, bool forceUnknown = false) {
    uint64_t occ = boardOccupancy();
    gSu = SetupEngine();
    gSu.tgt = target;
    gSu.want = cr::occupancy(target);
    gSu.black = (mode == SM_BLACK);
    gSu.prevOcc = occ;
    gSu.lastChange = millis();
    BootCheck bc = setupBootCheck(occ);
    gSu.unknown = forceUnknown || bc == BOOT_UNKNOWN;
    if (!gSu.unknown) memcpy(gSu.assumed, gLb.pieces, 64);
    else memset(gSu.assumed, 0, 64);
    gSu.cleared = true;                               // unused, kept for compatibility
    gSu.st = S_PLACE;
    DBG(SETUP, "begin: board=%s (saved check=%d diff=%d) user plays %s, target pieces=%d, sensors=%d",
        gSu.unknown ? "UNKNOWN -> fix one square at a time" : "KNOWN -> restore from saved map",
        (int)bc, lastBoardValid() ? __builtin_popcountll(occ ^ gLb.occ) : -1,
        gSu.black ? "black" : "white", __builtin_popcountll(gSu.want), __builtin_popcountll(occ));
    ledsClearOverlay();                                // drop flashes of the game that was running
    setupReplan(occ);
    SetupState ns = setupDerive();
    if (ns != gSu.st) DBG(SETUP, "state %s -> %s", setupStateName(gSu.st), setupStateName(ns));
    gSu.st = (ns == S_IDLE) ? S_PLACE : ns;
    setupDrawLeds();
    gSu.drawAt = millis();
}

// Feed the debounced occupancy (call from the UI task, e.g. every loop). Returns the state.
inline SetupState setupTick(uint64_t occ) {
    if (gSu.st == S_IDLE) return S_IDLE;
    uint32_t now = millis();
    bool changed = !gSu.planned || occ != gSu.occ;

    // what was just placed / lifted?
    if (occ != gSu.prevOcc) {
        uint64_t placed = occ & ~gSu.prevOcc;
        if (placed) {
            gSu.fbGood = placed & gSu.want;           // wanted square  -> green
            gSu.fbBad = placed & ~gSu.want;           // unwanted square -> red
            gSu.fbUntil = now + SETUP_FEEDBACK_MS;
        }
        gSu.prevOcc = occ;
        gSu.lastChange = now;
    }
    if (changed) setupReplan(occ);

    // stable match: occupancy equals the target for SENSOR_DEBOUNCE frames
    if (occ == gSu.want) {
        if (!gSu.matching) { gSu.matching = true; gSu.matchFrame0 = boardFrames(); }
        gSu.stable = (uint32_t)(boardFrames() - gSu.matchFrame0) >= SENSOR_DEBOUNCE;
    } else {
        gSu.matching = gSu.stable = gSu.confirmed = false;
    }

    SetupState ns = setupDerive();
    bool stChanged = ns != gSu.st;
    if (stChanged) {
        DBG(SETUP, "state %s -> %s (remaining=%d)", setupStateName(gSu.st), setupStateName(ns), (int)gSu.total);
        gSu.st = ns;
        if (ns == S_DONE) {                            // the board really shows the target now
            lastBoardSave(gSu.tgt);                    // RAM only
        }
    }

    // feedback timer + idle hint
    bool fbActive = gSu.fbUntil && (int32_t)(gSu.fbUntil - now) > 0;
    bool fbExpired = gSu.fbUntil && !fbActive;
    if (fbExpired) { gSu.fbUntil = 0; gSu.fbGood = gSu.fbBad = 0; }
    bool hint = (gSu.st == S_PLACE || gSu.st == S_RESTORE) && !fbActive && (now - gSu.lastChange >= SETUP_HINT_MS);
    bool hintChanged = hint != gSu.hintOn;
    gSu.hintOn = hint;

    uint32_t sig = (uint32_t)(occ ^ (occ >> 32)) * 31u + (uint32_t)gSu.st * 7u + (gSu.nSteps ? gSu.steps[0].sq * 64u + gSu.steps[0].to + gSu.steps[0].kind * 4096u : 0u);
    if (changed || stChanged || hintChanged || fbExpired || sig != gSu.drawSig || now - gSu.drawAt > 30000) {
        gSu.drawSig = sig;
        gSu.drawAt = now;
        setupDrawLeds();
    }
    return gSu.st;
}

// The user answered the S_VERIFY question. yes -> S_DONE.  no -> the saved map was wrong: forget it, start over.
inline void setupConfirm(bool yes) {
    if (gSu.st != S_VERIFY) return;
    DBG(SETUP, "user answered %s", yes ? "YES (position is right)" : "NO (start again, board treated as unknown)");
    if (yes) {
        gSu.confirmed = true;
        setupTick(gSu.occ);                            // -> S_DONE, saves the board
    } else {
        lastBoardInvalidate();
        setupBegin(gSu.tgt, gSu.black ? SM_BLACK : SM_WHITE, true);
    }
}

// Leave setup. The LEDs are cleared; call gameClaimBoard(slot) afterwards to repaint the game.
inline void setupEnd() {
    if (gSu.st == S_IDLE) return;
    DBG(SETUP, "end (state was %s)", setupStateName(gSu.st));
    gSu.st = S_IDLE;
    ledsClear();
    ledsClearOverlay();
}