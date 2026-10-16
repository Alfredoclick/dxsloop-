/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Andre Rezemini (the SLOOP dualcore branch, github.com/andrerezemini/sloop-lab) */
/* The render split over the two cores (fx.c mix_units): the same output, bit for bit, whatever the split.
 * Each scene renders about two seconds with cpu1_units = 0 (every unit on this core, as SLOOP 2.5 renders) and
 * again with every other split: each unit alone, pairs, all four, the firmware's own choice (-1); each render
 * in a fresh fork()ed child of a process that never touched the DSP state, as tests/regress.c does. The FNV-1a
 * hash of the output must be the one of 0 every time. sloopDX 4.0: the scenes on the DX7 and the FM drums, the units
 * sharing what they could share:
 *   dx3        three DX7 parts with chords (the operator buses: dx7_core.c, on the stack now), the drums (FM voices
 *              with noise operators, the drum bus with DRIVE and COMP), the metronome
 *   dxknobs    three DX7 parts, the quick knobs and the low-pass (CUT, RESO; ENV and LFO -> FLT) moved under the
 *              sounding notes (dx_follow rebuilds a voice), a VOICE change under them, the drum lanes' macros
 *   lfo        S&H LFOs on every part (rng() draws inside the render: those parts stay on core 0 at the wraps), DX7
 *              voices with their own LFO (PMS / AMS), pitch and amplitude
 *   mix        DIST (each TYPE, TONE, MIX) and the sends; mutes and a solo coming and going; the drums through the
 *              SLICER (GATE, then STUT); step locks of TUNE / DECAY on the drums; the metronome
 *   fx26       SLOOP 2.6 / 2.7: an INSERT on every part (SOFT, CRUSH, FLANG) and on the drum track (FUZZ), LFO 2 in SYNC
 *              with S&H waves, UNI and FREE, ENV SYNC on every part, a preset loaded under the sounding notes of
 *              two parts in one block (both fade with their old sound at once, one on each core), TEXTURE (VINYL,
 *              ducked), REV THROW then ECHO THROW, the punch-in FREEZE, a pitch bend, live drum mutes, the metronome
 * Runs under every mode of the host double of core 1 (tests/cpu1_host.c: CORES, CHAOS, CPU1_TIMEOUT_AT) and
 * under ThreadSanitizer (tests/cpu1_modes.sh). Prints the jobs handed to core 1, to show the split happened.
 *   build/host/mix_split_test [QUICK]       QUICK: the splits 0, 15 and -1 only (the TSan run) */
#define main hostsim_main
#include "hostsim.c"
#undef main

#define BLOCKS 1500u                                     /* ~2.2 s at 44.1 kHz */
static const int32_t MASKS[] = {0, -1, 1, 2, 4, 8, 3, 5, 12, 7, 15};
static const int32_t MASKS_QUICK[] = {0, 15, -1};
static const char *const SCENE_NAME[] = {"dx3", "dxknobs", "lfo", "mix", "fx26"};
#define NSCENE 5u

static void drums_pattern(void)                          /* kick 4 on the floor, snare 4 / 12, hats, a clap */
{
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        uint8_t n[4];
        uint32_t k = 0;
        if (i % 4u == 0u)
            n[k++] = 36;
        if (i == 4u || i == 12u)
            n[k++] = 38;
        if (i % 2u == 0u)
            n[k++] = i == 14u ? 46 : 42;
        if (i == 7u)
            n[k++] = 39;
        put_step(TDRUM, i, k, n, k ? ST_NOTE : ST_REST, i % 4u == 0u ? SF_ACCENT : 0u);
    }
}

static void chords(track_t *t, uint32_t first, uint32_t every, uint32_t base)   /* 3-note chords on a 16-step pattern */
{
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        uint8_t n[3] = {(uint8_t)(base + i), (uint8_t)(base + i + 4u), (uint8_t)(base + i + 7u)};
        int on = i >= first && (i - first) % every == 0u;
        put_step(t, i, on ? 3u : 0u, n, on ? ST_NOTE : ST_REST, i == first ? SF_ACCENT : 0u);
    }
}

static void scene_setup(uint32_t s)
{
    uint32_t k;
    host_tracks_init();
    song.g[G_BPM] = 140;
    song.g[G_DRLVL] = 90;
    song.g[G_DRREV] = 40;
    song.g[G_DRDLY] = 30;
    drums_pattern();
    switch (s) {
    case 0:
        for (k = 0; k < NPART; k++) {
            host_preset(&trk[k], 0, k == 0u ? 0u : k == 1u ? 5u : 6u);   /* EPIANO 1, BRASS, STRINGS */
            chords(&trk[k], k, 4, 48 + 5 * k);
        }
        song.g[G_CLOCK] = CLICK_ON;
        TDRUM->p[P_E0] = 1;                              /* the second kit */
        TDRUM->p[P_DIST] = 70;                           /* the drum bus: DRIVE */
        TDRUM->p[P_CHOR] = 90;                           /* .. and COMP */
        break;
    case 1:
        for (k = 0; k < NPART; k++) {
            host_preset(&trk[k], 0, 8u + k);             /* BELLS, MARIMBA, ORGAN */
            trk[k].p[P_E6] = 70;                         /* CUT */
            trk[k].p[P_E7] = 60;                         /* RESO */
            trk[k].p[P_ED_FLT] = 30;
            chords(&trk[k], k, 4, 52 + 3 * k);
        }
        dext.m[0][DM_TUNE] = -3, dext.m[2][DM_DECAY] = 20, dext.m[3][DM_NOISE] = 30, dext.m[6][DM_PAN] = -40;
        break;
    case 2:
        for (k = 0; k < NPART; k++) {
            host_preset(&trk[k], 0, k == 0u ? 6u : k == 1u ? 14u : 7u);   /* STRINGS, SAW LEAD, GLASS PAD */
            trk[k].p[P_LWAVE] = 4;                       /* S&H */
            trk[k].p[P_LRATE] = (int16_t)(110 + 5 * k);
            trk[k].p[P_LD_PIT] = 40;
            trk[k].p[P_LD_AMP] = 30;
            chords(&trk[k], k, 4, 50 + 4 * k);
        }
        break;
    case 4:
        host_preset(&trk[0], 0, 2);                      /* FM BASS */
        host_preset(&trk[1], 0, 0);                      /* EPIANO 1 */
        host_preset(&trk[2], 0, 10);                     /* ORGAN */
        for (k = 0; k < NPART; k++) {
            trk[k].p[P_ITYPE] = (int16_t)(k == 0u ? 1 : k == 1u ? 5 : 7);   /* SOFT, CRUSH, FLANG */
            trk[k].p[P_IA] = (int16_t)(60 + 20 * k);
            trk[k].p[P_IB] = 70;
            trk[k].p[P_IC] = (int16_t)(k == 1u ? 90 : 112);
            trk[k].p[P_IMIX] = (int16_t)(80 + 15 * k);
            trk[k].p[P_LWAVE] = 4;                       /* S&H: a draw at every wrap */
            trk[k].p[P_LSYNC] = (int16_t)(6 + k);        /* 1/8, 8T, 1/16 of the tempo */
            trk[k].p[P_LTRIG] = (int16_t)(k & 1u);       /* NOTE / FREE */
            trk[k].p[P_LPOL] = (int16_t)(k == 2u);       /* BI / UNI */
            trk[k].p[P_LD_PIT] = 30;
            trk[k].p[P_LD_AMP] = 40;
            trk[k].p[P_ESYNC] = 1;
            trk[k].p[P_ATK] = (int16_t)(10 + 30 * k);
            trk[k].p[P_DEC] = (int16_t)(50 + 20 * k);
            trk[k].p[P_REL] = (int16_t)(40 + 10 * k);
            trk[k].p[P_REV] = 60;
            trk[k].p[P_DLY] = 50;
            chords(&trk[k], k, 4, 48 + 6 * k);
        }
        TDRUM->p[P_ITYPE] = 4;                           /* FUZZ on the drums */
        TDRUM->p[P_IA] = 50;
        TDRUM->p[P_IB] = 100;
        TDRUM->p[P_IC] = 112;
        TDRUM->p[P_IMIX] = 100;
        song.g[G_CLOCK] = CLICK_ON;
        song.g[G_DUCK] = 60;
        song.g[G_TXSRC] = 1;                             /* VINYL */
        song.g[G_TXLVL] = 90;
        song.g[G_TXTONE] = -20;
        song.g[G_TXMOVE] = 80;
        song.g[G_TXRATE] = 0;
        song.g[G_TXWHEN] = 1;                            /* ALWAYS */
        song.g[G_TXDUCK] = 1;
        break;
    default:
        host_preset(&trk[0], 0, 3);                      /* SLAP BASS */
        host_preset(&trk[1], 0, 11);                     /* CLAV */
        host_preset(&trk[2], 0, 15);                     /* KOTO */
        trk[0].p[P_DIST] = 60, trk[0].p[P_DTYPE] = 2, trk[0].p[P_DTONE] = 20;
        trk[1].p[P_DIST] = 50, trk[1].p[P_DTYPE] = 3, trk[1].p[P_DMIX] = 90;
        trk[1].p[P_REV] = 90;
        trk[2].p[P_CHOR] = 80;
        trk[2].p[P_DLY] = 70;
        for (k = 0; k < NPART; k++)
            chords(&trk[k], k, 4, 45 + 7 * k);
        song.g[G_CLOCK] = CLICK_ON;
        song.g[G_CMIX] = 90, song.g[G_RPRE] = 30;
        TDRUM->p[P_SLCR] = SL_GATE;
        TDRUM->p[P_SLPAT] = 3;
        TDRUM->p[P_SLDEPTH] = 100;
        dext.lock[2] = dlock_make(2, 1, 5, 1, -20);      /* a TUNE / DECAY lock on the snare of step 3 */
        dext.lock[8] = dlock_make(0, 1, -4, 0, 0);
        break;
    }
    transport_req = 1;
}

static void scene_block(uint32_t s, uint32_t b)          /* what the UI would do between blocks */
{
    uint32_t k;
    if (s == 1u) {
        if (b % 100u == 50u)
            for (k = 0; k < NPART; k++) {                /* the quick knobs under the notes (dx_follow) */
                trk[k].p[P_E1] = (int16_t)((int32_t)(b / 100u) * 7 % 81 - 40);   /* BRITE */
                trk[k].p[P_E5] = (int16_t)((int32_t)(b / 100u + k) * 11 % 81 - 40);   /* FDBK */
                trk[k].p[P_E6] = (int16_t)(40 + (b / 100u) * 9 % 88);   /* CUT */
            }
        if (b == 700u)
            trk[1].p[P_E0] = 12;                         /* another VOICE under the notes (PLUCK) */
        if (b == 900u)
            dext.m[2][DM_TUNE] = 5, dext.m[4][DM_BRIGHT] = -20;
    }
    if (s == 3u) {
        if (b % 128u == 64u)
            trk[(b / 128u) % NPART].p[P_MUTE] ^= 1;      /* mute fades, each part in turn */
        if (b == 500u)
            song.solo = 2;
        if (b == 700u)
            song.solo = 0;
        if (b == 600u)
            TDRUM->p[P_SLCR] = SL_STUT;
        if (b == 1000u)
            trk[0].p[P_DTYPE] = 0;
    }
    if (s == 4u) {
        if (b == 300u)
            thr_req = 1;                                 /* REV THROW held .. */
        if (b == 420u)
            thr_req = 2;                                 /* .. then ECHO THROW */
        if (b == 560u)
            thr_req = 0;
        if (b == 500u) {                                 /* a preset on two parts under their notes (ui.c apply_preset_to):
                                                          * both fade out with their old sound in the same blocks */
            for (k = 1; k < NPART; k++) {
                sound_change(&trk[k]);
                panic_req |= (uint8_t)(1u << k);
                host_preset_req(&trk[k], 0, 4 + k);
            }
        }
        if (b == 700u)
            punch.req = PX_FREEZE;                       /* the reverb held, the dry mix fed into it */
        if (b == 900u)
            punch.req = -1;
        if (b == 1000u)
            drum_mute = 0x5;                             /* lanes 0 and 2 muted live (EDIT + a black key) */
        if (b == 1100u)
            trk[0].bend = 5000;
        if (b == 1200u)
            drum_mute = 0;
    }
}

typedef struct { uint64_t hash; uint32_t jobs, timeouts; } res_t;

static res_t render(uint32_t s, int32_t mask)
{
    res_t r = {0xcbf29ce484222325ull, 0, 0};
    uint32_t b, i, k;
    cpu1_host_mode();                                    /* init: the env (CORES, CHAOS, CPU1_TIMEOUT_AT) */
    cpu1_units = mask;
    scene_setup(s);
    for (b = 0; b < BLOCKS; b++) {
        int32_t o[2 * CTL];
        scene_block(s, b);
        mix_block(o, CTL);
        for (i = 0; i < 2u * CTL; i++) {
            uint32_t u = (uint32_t)o[i];
            for (k = 0; k < 4u; k++) {
                r.hash ^= (u >> (8u * k)) & 0xFFu;
                r.hash *= 0x100000001B3ull;
            }
        }
    }
    r.jobs = cpu1_host_jobs();
    r.timeouts = cpu1_host_timeouts();
    return r;
}

static int child(uint32_t s, int32_t mask, res_t *out)  /* the render in a fresh process; 0 = it ran */
{
    int p[2], st = 0;
    pid_t pid;
    size_t got = 0;
    ssize_t n;
    if (pipe(p))
        return -1;
    fflush(stdout);
    if (!(pid = fork())) {
        res_t r = render(s, mask);
        close(p[0]);
        if (write(p[1], &r, sizeof r) != (ssize_t)sizeof r)
            _exit(1);
        _exit(0);
    }
    close(p[1]);
    while (got < sizeof *out && (n = read(p[0], (char *)out + got, sizeof *out - got)) > 0)
        got += (size_t)n;
    close(p[0]);
    waitpid(pid, &st, 0);
    return got == sizeof *out && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    int quick = argc > 1 && !strcmp(argv[1], "QUICK"), fails = 0;
    const int32_t *masks = quick ? MASKS_QUICK : MASKS;
    uint32_t nm = quick ? sizeof MASKS_QUICK / sizeof MASKS_QUICK[0] : sizeof MASKS / sizeof MASKS[0], s, m;
    const char *e = getenv("CPU1_TIMEOUT_AT");
    uint32_t lost = e ? (uint32_t)atoi(e) : 0;
    for (s = 0; s < NSCENE; s++) {
        res_t ref;
        if (child(s, 0, &ref)) {
            printf("split: %-9s no split: CRASH\n", SCENE_NAME[s]);
            fails++;
            continue;
        }
        for (m = 0; m < nm; m++) {
            res_t r;
            int32_t mask = masks[m];
            const char *why = 0;
            if (child(s, mask, &r))
                why = "crashed";
            else if (r.hash != ref.hash)
                why = "the output differs from the unsplit render";
            else if (mask && !r.jobs)
                why = "no job was handed to core 1";
            else if (!mask && r.jobs)
                why = "jobs handed to core 1 with no unit for it";
            else if (lost && mask && r.jobs > lost)
                why = "jobs handed over after the lost one (core 1 should be held)";
            printf("split: %-9s units %3d  %016llx  jobs %4u  timeouts %u  %s\n", SCENE_NAME[s], mask,
                   (unsigned long long)r.hash, r.jobs, r.timeouts, why ? why : "same");
            if (why)
                fails++;
        }
    }
    printf("render split: %u scenes x %u splits, CORES=%d CHAOS=%s CPU1_TIMEOUT_AT=%u: %s\n", NSCENE, nm, cpu1_host_mode(),
           getenv("CHAOS") ? getenv("CHAOS") : "0", lost, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
