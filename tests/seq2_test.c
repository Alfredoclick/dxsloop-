/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP 2.0 sequencer: the transport clock and what plays on it.
 *   timing   the kick and the click stay together for 64 bars at any tempo (no drift); a tempo or DIV
 *            change mid-step plays one step, not a burst; LEN changes keep the track in phase; an odd
 *            LEN with SWING stays in phase with a 16-step track
 *   ratchet  x2 / x3 / x4 hits evenly in their step (drums and synth)
 *   roll     ARP + key: hits on the grid at G_ROLL; recorded as ratchets (1/32 -> x2 on 1/16 steps)
 *   erase    EDIT + key: the lane goes as the playhead passes (playing), all of it (stopped); undo / redo
 *   levels   OCT- / OCT+ held on the drum track: ghost / hard hits, recorded as such
 *   chords   P_CHORD: one key plays the chord of the scale (white keys walk the degrees from C4)
 *   mute     P_MUTE / solo: no new notes, the output fades
 *   2.4      (SLOOP 2.4, in sloopDX 3.3) a MIDI START cuts the count-in short, no swing on triplets, a DIV change
 *            keeps the next step, DIV 1/2 / 1BAR / 2BAR, dotted delays, the sequencer to MIDI OUT, MIDI IN = CLOCK
 *   3.4      (SLOOP 2.5) MIDI CCs, the drums' delay send, the click with the drum track muted
 * Exit status: the number of failed checks. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <assert.h>

static uint64_t blk;
typedef struct { uint64_t blk; uint8_t note, vel; } hit_t;
static hit_t hits[200000];
static uint32_t nhits;
static void run_block(void)
{
    int32_t out[CTL * 2];
    uint32_t a = drums.age, k;
    mix_block(out, CTL);
    if (drums.age != a)
        for (k = 0; k < NDRUM; k++)
            if (drums.v[k].age > a && nhits < 200000u) {
                hits[nhits].blk = blk;
                hits[nhits].note = drums.v[k].note;
                hits[nhits].vel = drums.v[k].vel;
                nhits++;
            }
    blk++;
}
static void reset(uint32_t bpm)
{
    uint32_t i;
    host_tracks_init();
    for (i = 0; i < NTRK; i++)
        steps_clear(&trk[i]);
    memset(&drums, 0, sizeof drums);
    
    nhits = 0;
    blk = 0;
    song.g[G_BPM] = (int16_t)bpm;
    song.rec = 0;
    rec_wait = 0;
    fm1_in.notes = fm1_in.buttons = 0;
    kb_prev = 0;
    ly_bit[LY_ROLL] = 1u << 8;                       /* (the buttons: any bits) */
    ly_bit[LY_ERASE] = 1u << 4;
    dyn_bit[0] = 1u << 0;
    dyn_bit[1] = 1u << 1;
}
static int fails;
static void check(int ok, const char *what)
{
    printf("seq2: %-72s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}
static uint32_t count_note(uint32_t note, uint64_t b0, uint64_t b1)
{
    uint32_t i, n = 0;
    for (i = 0; i < nhits; i++)
        n += hits[i].note == note && hits[i].blk >= b0 && hits[i].blk < b1;
    return n;
}

static void t_drift(void)
{
    static const uint32_t BPM[5] = {87, 90, 120, 128, 174};
    uint32_t bi, worst = 0;
    for (bi = 0; bi < 5u; bi++) {
        uint32_t i, nk = 0, nc = 0, bad = 0;
        uint64_t kb[80], cb[80];
        reset(BPM[bi]);
        song.g[G_CLOCK] = 2;                         /* the click on: its downbeat (77) */
        dstep_set(&TDRUM->dstep[0], 0, LV_NORM, 0);  /* a kick on the 1 */
        transport_req = 1;
        while (blk < (uint64_t)(FS * 60.0 / BPM[bi] * 4 * 66 / CTL))
            run_block();
        for (i = 0; i < nhits; i++) {
            if (hits[i].note == 36 && nk < 80u)
                kb[nk++] = hits[i].blk;
            if (hits[i].note == 77 && nc < 80u)
                cb[nc++] = hits[i].blk;
        }
        for (i = 0; i < nk && i < nc && i < 64u; i++) {
            uint32_t d = kb[i] > cb[i] ? (uint32_t)(kb[i] - cb[i]) : (uint32_t)(cb[i] - kb[i]);
            bad += d != 0u;
            worst = d > worst ? d : worst;
        }
        if (bad || nk < 64u || nc < 64u)
            printf("seq2:   %u BPM: %u kicks, %u clicks, %u apart\n", BPM[bi], nk, nc, bad);
        worst += (nk < 64u || nc < 64u) * 1000u;
    }
    check(worst == 0u, "64 bars at 87..174 BPM: every kick in the block of its downbeat click");
}

static void t_burst(void)
{
    uint32_t i, n0;
    int ok1, ok2;
    reset(90);
    TDRUM->p[P_SDIV] = 0;                            /* 1/4 */
    for (i = 0; i < 16u; i++)
        dstep_set(&TDRUM->dstep[i], 4, LV_NORM, 0);
    transport_req = 1;
    while (clk_beat < 2u || clk_pos < BEAT_U * 85u / 100u)
        run_block();
    TDRUM->p[P_SDIV] = 3;                            /* DIV 1/4 -> 1/32 at 85 % of a step */
    n0 = nhits;
    run_block();
    ok1 = nhits - n0 <= 1u;
    reset(40);
    for (i = 0; i < 16u; i++)
        dstep_set(&TDRUM->dstep[i], 4, LV_NORM, 0);
    transport_req = 1;
    while (clk_beat < 1u || clk_pos < BEAT_U / 4u * 3u / 4u)
        run_block();
    song.g[G_BPM] = 240;                             /* 40 -> 240 in the middle of a step */
    n0 = nhits;
    run_block();
    ok2 = nhits - n0 <= 1u;
    check(ok1 && ok2, "DIV 1/4 -> 1/32 and 40 -> 240 BPM mid-step: one step a block, no burst");
}

static void t_swing_odd(void)
{
    uint32_t i, ok = 1, starts0 = 0, startsd = 0;
    uint64_t at0[64], atd[64];
    reset(120);
    song.g[G_SWING] = 50;
    trk[0].p[P_SLEN] = 3;
    TDRUM->p[P_SLEN] = 16;
    transport_req = 1;
    for (i = 0; i < 20u * FS / CTL; i++) {
        uint32_t b0 = trk[0].seq_idx, bd = TDRUM->seq_idx;
        uint32_t f0 = trk[0].seq_abs, fd = TDRUM->seq_abs;
        run_block();
        if (trk[0].seq_abs != f0 && trk[0].seq_idx == 0u && starts0 < 64u)
            at0[starts0++] = blk - 1u;
        if (TDRUM->seq_abs != fd && TDRUM->seq_idx == 0u && startsd < 64u)
            atd[startsd++] = blk - 1u;
        (void)b0;
        (void)bd;
    }
    /* step 48 = cycle 16 of 3 = bar 3 of 16; step 144 = cycle 48 = bar 9 */
    ok = starts0 > 48u && startsd > 9u && at0[16] == atd[3] && at0[48] == atd[9];
    if (!ok)
        printf("seq2:   %u %u starts; step 48: %llu %llu, step 144: %llu %llu\n", starts0, startsd,
               (unsigned long long)at0[16], (unsigned long long)atd[3], (unsigned long long)at0[48], (unsigned long long)atd[9]);
    check(ok, "SWING 75 %, LEN 3 against LEN 16: in phase after 15 bars (no drift)");
}

static void t_len_phase(void)
{
    reset(120);
    TDRUM->p[P_SLEN] = 32;
    trk[0].p[P_SLEN] = 16;
    transport_req = 1;
    while (TDRUM->seq_idx != 20u)
        run_block();
    TDRUM->p[P_SLEN] = 16;                           /* LEN 32 -> 16 at step 20 */
    while (TDRUM->seq_idx == 20u)
        run_block();
    check(TDRUM->seq_idx == trk[0].seq_idx, "LEN 32 -> 16 while playing: in phase with a 16-step track");
}

static void t_ratchet(void)
{
    uint32_t i, n = 0, gaps_ok = 1;
    uint64_t b[8];
    reset(120);
    dstep_set(&TDRUM->dstep[0], 4, LV_NORM, 3);      /* a hat x4 on step 0 */
    dstep_set(&TDRUM->dstep[1], 0, LV_NORM, 0);      /* a kick on step 1 */
    transport_req = 1;
    while (blk < div_samples(2) * 2u / CTL + 2u)
        run_block();
    for (i = 0; i < nhits; i++)
        if (hits[i].note == 42 && n < 8u)
            b[n++] = hits[i].blk;
    for (i = 1; i < n; i++) {
        int32_t g = (int32_t)(b[i] - b[i - 1]), want = (int32_t)(div_samples(2) / 4u / CTL);
        gaps_ok &= g >= want - 1 && g <= want + 1;
    }
    check(n == 4u && gaps_ok && count_note(36, 0, blk) == 1u, "drums: a hat x4 plays 4 even hits in its step, the next step plays");
    reset(120);
    host_preset(&trk[0], 0, 7);                      /* TRAP PLUCK (POLY) */
    put_step(&trk[0], 0, 1, (const uint8_t[]){60}, ST_NOTE, 0);
    trk[0].step[0].rat = 2;                          /* x3 */
    transport_req = 1;
    {
        uint32_t starts = 0, last = vage;
        while (blk < div_samples(2) / CTL) {
            run_block();
            if (vage != last)
                starts += vage - last;
            last = vage;
        }
        check(starts == 3u, "synth: a note x3 starts 3 times in its step");
    }
}

static void t_roll(void)
{
    uint32_t i, k = 6, n;                           /* key 6 = B3 -> lane 3 (clap) */
    reset(120);
    song.g[G_ROLL] = 2;                              /* 1/32 */
    song.sel = TRK_DRUM;
    song.rec = 1u << TRK_DRUM;
    transport_req = 1;
    run_block();
    fm1_in.buttons = ly_bit[LY_ROLL];                /* ARP held */
    fm1_in.notes = 1u << k;
    for (i = 0; i < div_samples(2) * 8u / CTL; i++)  /* 8 steps */
        run_block();
    fm1_in.notes = 0;
    fm1_in.buttons = 0;
    run_block();
    n = count_note(LANE_NOTE[3], 0, blk);
    {
        uint32_t s, rat_ok = 1, on = 0;
        for (s = 1; s < 7u; s++) {
            on += dstep_has(&TDRUM->dstep[s], 3);
            rat_ok &= !dstep_has(&TDRUM->dstep[s], 3) || dstep_rat(&TDRUM->dstep[s], 3) == 1u;
        }
        if (n < 15u || n > 17u)
            printf("seq2:   roll: %u hits\n", n);
        check(n >= 15u && n <= 17u, "roll 1/32 held 8 steps: ~16 hits");
        check(on >= 6u && rat_ok, "roll 1/32 recorded: one hit a step, ratchet x2");
    }
}

static void t_erase_undo(void)
{
    uint32_t i, left = 0, kept = 0;
    reset(120);
    for (i = 0; i < 16u; i++) {
        dstep_set(&TDRUM->dstep[i], 4, LV_NORM, 0);  /* hats everywhere */
        if (i % 4u == 0u)
            dstep_set(&TDRUM->dstep[i], 0, LV_NORM, 0);
    }
    song.sel = TRK_DRUM;
    transport_req = 1;
    while (TDRUM->seq_idx != 4u)
        run_block();
    fm1_in.buttons = ly_bit[LY_ERASE];               /* EDIT held + the hat key (C4 = key 7) */
    fm1_in.notes = 1u << 7;
    while (TDRUM->seq_idx != 7u)
        run_block();
    for (i = 0; i < 20u; i++)                        /* (into step 7, not 8) */
        run_block();
    fm1_in.notes = 0;
    fm1_in.buttons = 0;
    run_block();
    for (i = 0; i < 16u; i++) {
        left += dstep_has(&TDRUM->dstep[i], 4);
        kept += dstep_has(&TDRUM->dstep[i], 0);
    }
    if (left != 12u)
        printf("seq2:   erase: %u hats left, %u kicks; %d %d\n", left, kept, dstep_has(&TDRUM->dstep[5], 4), dstep_has(&TDRUM->dstep[9], 4));
    check(left == 12u && kept == 4u && !dstep_has(&TDRUM->dstep[5], 4) && dstep_has(&TDRUM->dstep[9], 4),
          "EDIT + hat while playing steps 4..7: those hats gone, the rest and the kicks kept");
    {   /* undo (ui.c undo_swap): the hats back */
        step_t tmp[NSTEP];
        memcpy(tmp, TDRUM->step, sizeof tmp);
        memcpy(TDRUM->step, undo.st, sizeof tmp);
        memcpy(undo.st, tmp, sizeof tmp);
    }
    left = 0;
    for (i = 0; i < 16u; i++)
        left += dstep_has(&TDRUM->dstep[i], 4);
    check(left == 16u && undo.trk == TRK_DRUM, "undo: the erased hats are back");
    transport_req = 2;
    run_block();
    fm1_in.buttons = ly_bit[LY_ERASE];               /* stopped: every hat of the pattern */
    fm1_in.notes = 1u << 7;
    run_block();
    fm1_in.notes = fm1_in.buttons = 0;
    run_block();
    left = 0;
    for (i = 0; i < 16u; i++)
        left += dstep_has(&TDRUM->dstep[i], 4);
    check(left == 0u, "EDIT + hat while stopped: every hat gone");
}

static void t_levels(void)
{
    uint32_t i, gh = 0, hd = 0;
    reset(120);
    song.sel = TRK_DRUM;
    song.rec = 1u << TRK_DRUM;
    transport_req = 1;
    run_block();
    fm1_in.buttons = dyn_bit[0];                     /* OCT- held: a ghost snare (A3 = key 4) */
    fm1_in.notes = 1u << 4;
    run_block();
    fm1_in.notes = 0;
    run_block();
    while (TDRUM->seq_idx != 3u)
        run_block();
    fm1_in.buttons = dyn_bit[1];                     /* OCT+ held: a hard kick */
    fm1_in.notes = 1u << 0;
    run_block();
    fm1_in.notes = fm1_in.buttons = 0;
    run_block();
    for (i = 0; i < 16u; i++) {
        if (dstep_has(&TDRUM->dstep[i], 2))
            gh = dstep_lvl(&TDRUM->dstep[i], 2);
        if (dstep_has(&TDRUM->dstep[i], 0))
            hd = dstep_lvl(&TDRUM->dstep[i], 0);
    }
    check(gh == LV_GHOST && hd == LV_HARD, "OCT- / OCT+ held on the drums: a ghost snare and a hard kick recorded");
    {
        uint32_t vg = 0, vh = 0;
        for (i = 0; i < nhits; i++) {
            if (hits[i].note == 38)
                vg = hits[i].vel;
            if (hits[i].note == 36)
                vh = hits[i].vel;
        }
        check(vg == 42u && vh == 127u, "... and played at 42 / 127");
    }
}

static void t_chords(void)
{
    uint8_t c[4];
    uint32_t n;
    reset(120);
    trk[0].p[P_ROOT] = 0;
    trk[0].p[P_SCALE] = 2;                           /* C minor */
    trk[0].p[P_CHORD] = 3;                           /* 9TH: 1 3 7 9 */
    n = chord_notes(&trk[0], kb_map(&trk[0], 7), c); /* C4: i */
    check(n == 4u && c[0] == 60 && c[1] == 63 && c[2] == 70 && c[3] == 74, "chord 9TH on C4 in C minor: C Eb Bb D");
    trk[0].p[P_CHORD] = 1;
    n = chord_notes(&trk[0], kb_map(&trk[0], 9), c); /* D4: ii (dim) */
    check(n == 3u && c[0] == 62 && c[1] == 65 && c[2] == 68, "chord TRIAD on D4 in C minor: D F Ab");
    check(kb_map(&trk[0], 8) == KB_SILENT, "chord mode: the black keys are silent");
}

static void t_mute(void)
{
    uint32_t i, n0;
    reset(120);
    for (i = 0; i < 16u; i++)
        dstep_set(&TDRUM->dstep[i], 0, LV_NORM, 0);
    transport_req = 1;
    while (blk < 200u)
        run_block();
    TDRUM->p[P_MUTE] = 1;
    n0 = nhits;
    while (blk < 400u)
        run_block();
    check(nhits == n0, "drum track muted: no hits");
    TDRUM->p[P_MUTE] = 0;
    song.solo = 1u;                                  /* track 1 soloed: the drums silent */
    n0 = nhits;
    while (blk < 600u)
        run_block();
    check(nhits == n0 && TDRUM->att == 32767, "track 1 soloed: the drums silent (faded out)");
    song.solo = 0;
    while (blk < 800u)
        run_block();
    check(nhits > n0 && TDRUM->att == 0, "solo off: the drums back");
}

/* overload shedding (voice.c shed_voice): a releasing voice first, then the oldest held voice that is
 * neither a POLY part's lowest note nor a MONO part's lead; faded (stage 4), never cut */
static void t_shed(void)
{
    track_t *t = &trk[0], *m = &trk[1];
    uint32_t i, k, low_ok = 1, lead_ok = 1;
    reset(120);
    t->p[P_VOICE] = V_POLY;
    m->p[P_VOICE] = V_MONO;
    t->p[P_ATK] = 0;
    t->p[P_REL] = 100;                                /* a long release: the released voice still rings */
    trk_note_on(t, 48, 100);                          /* the bass, first and lowest */
    trk_note_on(t, 64, 100);
    trk_note_on(t, 67, 100);
    trk_note_on(t, 72, 100);
    trk_note_on(m, 40, 100);                          /* a MONO lead */
    trk_note_on(t, 76, 100);
    for (k = 0; k < 20u; k++)
        run_block();
    trk_note_off(t, 76);                              /* releasing */
    run_block();
    shed_voice();
    for (k = 0, i = 0; i < NVOICE; i++)
        k += t->v[i].note == 76 && t->v[i].stage == 4u;
    check(k == 1u, "overload: the releasing voice goes first, faded (stage 4)");
    for (k = 0; k < 6u; k++)
        shed_voice();
    for (i = 0; i < NVOICE; i++) {
        if (t->v[i].note == 48 && t->v[i].active && t->v[i].stage == 4u)
            low_ok = 0;
        if (m->v[i].note == 40 && i == 0u && m->v[i].stage == 4u)
            lead_ok = 0;
    }
    for (k = 0, i = 0; i < NVOICE; i++)
        k += t->v[i].active && t->v[i].gate && (t->v[i].note == 64 || t->v[i].note == 67 || t->v[i].note == 72);
    check(low_ok && lead_ok && k == 0u, "overload: the upper notes thin out, the bass and the MONO lead stay");
}

/* MIDI clock in (SYNC USB / TRS): START, 24 pulses a beat, tempo changes, STOP; the other source ignored */
static void mclk_push(uint32_t pkt) { midi_in_q[mi_w % MQ] = pkt; mi_w++; }
static void mclk_run(double *t, double *next, double per, double until, uint32_t src)
{
    while (*t < until) {
        while (*next <= *t) {
            mclk_push(0xF80Fu | src << 4);
            *next += per;
        }
        fm1_ms = (uint32_t)*t;
        run_block();
        *t += (double)CTL * 1000.0 / FS;
    }
}
static void t_mclk(void)
{
    double t = 0, next = 0, per = 60000.0 / 120 / 24;
    reset(90);                                        /* the internal tempo: 90 */
    mi_r = mi_w;
    song.g[G_SYNC] = 1;                               /* USB */
    mclk_run(&t, &next, per, 700, 0);                 /* the clock runs before START (the tempo shows) */
    check(!song.playing && song.g[G_BPM] == 120, "MIDI clock: stopped, BPM follows the master (120)");
    mclk_push(0xFA0Fu);                               /* START, then the downbeat pulse */
    next = t;
    mclk_run(&t, &next, per, t + 4000.0 + per / 2, 0);   /* two bars of 120 */
    check(song.playing && clk_beat == 8u, "MIDI clock: START, 2 bars at 120 -> beat 8 exactly");
    per = 60000.0 / 100 / 24;                          /* the master slows to 100 */
    mclk_run(&t, &next, per, t + 4800.0, 0);
    check(clk_beat == 16u && song.g[G_BPM] == 100, "MIDI clock: the master at 100 -> 2 more bars, BPM 100");
    mclk_push(0xFC1Fu);                               /* STOP from the TRS jack: not the source */
    mclk_run(&t, &next, per, t + 50.0, 0);
    check(song.playing, "MIDI clock: SYNC USB ignores the TRS jack");
    mclk_push(0xFC0Fu);
    mclk_run(&t, &next, per, t + 10.0, 0);
    check(!song.playing, "MIDI clock: STOP");
    t += 1000.0;                                      /* the master is gone: PLAY on the FM-1 plays */
    fm1_ms = (uint32_t)t;
    transport_req = 1;
    run_block();
    {
        uint32_t b0 = clk_beat, k;
        for (k = 0; k < (uint32_t)(FS / CTL); k++)
            run_block();
        check(song.playing && clk_beat >= b0 + 1u, "MIDI clock: no pulse for 0.5 s -> the internal tempo plays");
    }
    transport_req = 2;
    run_block();
    song.g[G_SYNC] = 0;
    mi_r = mi_w;
}

/* the REC screen's MODE and START (SLOOP 2.3): an empty project records at the tempo set (TEMPO) or
 * takes it from the playing (FREE); COUNT: PLAY clicks one bar, then the loop and the recording start */
static void t_recmode(void)
{
    uint32_t k, bpb = (uint32_t)((double)FS * 60.0 / 100.0 / CTL + 0.5), c0;
    reset(100);
    song.sel = 0;
    song.playing = 0;
    rec_tempo = 0, rec_count = 0;
    rec_wait = 1;
    input_on(TSEL, 60, 100);
    run_block();
    check(ft_on && !song.playing, "REC mode FREE, empty project: the first note starts a free take");
    transport_req = 2; run_block(); ft_bars = 0;
    for (k = 0; k < 4u; k++) trk_note_off(&trk[k % NPART], 60);

    reset(100);
    song.sel = 0;
    rec_tempo = 1, rec_count = 0;
    rec_wait = 1;
    input_on(TSEL, 60, 100);
    run_block();
    check(!ft_on && song.playing && song.rec == 1u && song.g[G_BPM] == 100,
          "REC mode TEMPO, empty project: the first note starts the loop at 100 BPM, recording");
    transport_req = 2; run_block();

    reset(100);
    song.sel = 0;
    rec_tempo = 1, rec_count = 1;
    rec_wait = 1;
    input_on(TSEL, 62, 100);
    run_block();
    check(!song.playing && !ci_on && !ft_on && rec_wait, "REC START COUNT: a note only sounds, nothing starts");
    c0 = nhits;
    transport_req = 1;                                /* PLAY: the count-in */
    run_block();
    for (k = 0; k + 2u < 4u * bpb; k++) run_block();
    check(ci_on && !song.playing, "REC START COUNT: PLAY -> one bar of clicks, not playing yet");
    {
        uint32_t n = 0, i;
        for (i = c0; i < nhits; i++) n += hits[i].note == 76 || hits[i].note == 77;
        check(n == 4u, "REC START COUNT: 4 clicks (one bar of 4/4)");
    }
    for (k = 0; k < 4u; k++) run_block();
    check(!ci_on && song.playing && song.rec == 1u && !rec_wait,
          "REC START COUNT: after the bar the loop starts and records");
    transport_req = 2; run_block();

    reset(100);
    song.sel = 0;
    rec_tempo = 1, rec_count = 1;
    rec_wait = 1;
    transport_req = 1; run_block();
    for (k = 0; k < bpb; k++) run_block();
    rec_wait = 0;                                     /* REC again: cancelled */
    for (k = 0; k < 4u * bpb; k++) run_block();
    check(!ci_on && !song.playing && !song.rec, "REC START COUNT: REC during the count-in cancels it");
    rec_wait = 1;
    transport_req = 1; run_block();
    transport_req = 1; run_block();                   /* PLAY again: back to armed */
    check(!ci_on && rec_wait && !song.playing, "REC START COUNT: PLAY again during the count-in: back to armed");
    rec_wait = 0;
    rec_tempo = 0, rec_count = 0;
}

/* menu USB AUDIO = FULL (2.3): the USB input at the level of MASTER all the way up, whatever the knob;
 * the DAC path keeps following the knob. sloopDX: a 3-note STRINGS chord (the DX7's E-piano attack has a
 * high crest factor: 8 notes at 110 drive the limiter on both paths, which hides the knob) */
static void t_usbfull(void)
{
    uint32_t k, i;
    int32_t out[CTL * 2], pk_dac = 0, pk_usb = 0;
    reset(120);
    song.master_q12 = 512;                            /* MASTER low */
    master_cur = -1;
    usb_full_now = 1;
    trk[0].p[P_E0] = 6;                               /* STRINGS */
    for (k = 0; k < 3u; k++) trk_note_on(&trk[0], 48u + k * 3u, 110);
    for (k = 0; k < 600u; k++) {
        mix_block(out, CTL);
        if (k > 100u)
            for (i = 0; i < 2u * CTL; i++) {
                int32_t a = out[i] < 0 ? -out[i] : out[i], u = usb_out[i] < 0 ? -usb_out[i] : usb_out[i];
                if (a > pk_dac) pk_dac = a;
                if (u > pk_usb) pk_usb = u;
            }
    }
    usb_full_now = 0;
    for (k = 0; k < 3u; k++) trk_note_off(&trk[0], 48u + k * 3u);
    song.master_q12 = 2048;
    check(pk_usb > pk_dac * 3 && pk_usb <= 32767, "USB AUDIO FULL: the USB level stays up when MASTER is low, no clipping");
}

/* ---- from SLOOP 2.4 (tests/seq2_test.c), ported with the features (sloopDX 3.3) */

/* SLOOP 2.4 fixes: a MIDI START cuts a count-in short (the master counts); swing is off on the
 * triplet grids; a DIV change in the first beat after PLAY does not lose a step */
static void t_fixes24(void)
{
    uint32_t k, i, bpb = (uint32_t)((double)FS * 60.0 / 100.0 / CTL + 0.5), n0;
    /* MIDI START during the count-in */
    transport_req = 2; run_block();                   /* (stopped: t_mute leaves it playing) */
    reset(100);
    mclk.alive = 0;                                   /* (t_mclk's master is gone: no clock drives yet) */
    song.sel = 0;
    rec_tempo = 1, rec_count = 1;
    rec_wait = 1;
    song.g[G_SYNC] = 1;
    mi_r = mi_w;
    transport_req = 1; run_block();                   /* PLAY: the count-in clicks */
    for (k = 0; k < bpb; k++) run_block();
    if (!(ci_on && !song.playing)) printf("seq2:   ci_on %u playing %u rec_wait %u empty %d mclk_on %d ft_on %u\n", (unsigned)ci_on,
        (unsigned)song.playing, (unsigned)rec_wait, project_empty(), mclk_on(), (unsigned)ft_on);
    check(ci_on && !song.playing, "2.4: count-in running before the master starts");
    mclk_push(0xFA0Fu);                               /* the DAW starts: START */
    run_block();
    run_block();
    check(!ci_on && song.playing && song.rec == 1u, "2.4: a MIDI START during the count-in starts and records at once");
    transport_req = 2; run_block();
    song.g[G_SYNC] = 0;
    rec_wait = 0; rec_tempo = 0; rec_count = 0;
    mi_r = mi_w;

    /* swing on 8T: every beat the same */
    reset(120);
    song.g[G_SWING] = 80;
    TDRUM->p[P_SDIV] = 4;                             /* 8T: 3 a beat */
    TDRUM->p[P_SLEN] = 12;
    for (i = 0; i < 12u; i++)
        dstep_set(&TDRUM->dstep[i], 4, LV_NORM, 0);
    transport_req = 1;
    for (k = 0; k < 8u * (uint32_t)(FS / CTL) / 2u; k++) run_block();   /* 4 s = 8 beats */
    {
        uint64_t d[24]; uint32_t n = 0, ok = 1;
        for (i = 0; i < nhits && n < 24u; i++)
            if (hits[i].note == 36 || hits[i].note == 35 || hits[i].vel) d[n++] = hits[i].blk;
        /* 3 hits a beat, evenly spaced: consecutive gaps within one block of each other */
        for (i = 2; i < n; i++)
            if (d[i] - d[i - 1] > d[i - 1] - d[i - 2] + 1u || d[i - 1] - d[i - 2] > d[i] - d[i - 1] + 1u) ok = 0;
        check(n >= 20u && ok, "2.4: SWING on the 8T grid does nothing (triplets stay even)");
    }
    song.g[G_SWING] = 0;
    transport_req = 2; run_block();

    /* DIV 1/8 -> 1/4 in the first beat: beat 2 still plays */
    reset(120);
    TDRUM->p[P_SDIV] = 1;                             /* 1/8 */
    TDRUM->p[P_SLEN] = 16;
    for (i = 0; i < 16u; i++)
        dstep_set(&TDRUM->dstep[i], 4, LV_NORM, 0);
    transport_req = 1;
    run_block();                                      /* (the clock is at 0 from here) */
    while (clk_beat < 1u && clk_pos < BEAT_U * 6u / 10u) run_block();   /* 60 % into beat 1: abs 1 of 1/8 */
    TDRUM->p[P_SDIV] = 0;                             /* 1/4 */
    n0 = nhits;
    while (clk_beat < 1u) run_block();                /* to the start of beat 2 */
    for (k = 0; k < 3u; k++) run_block();
    check(nhits > n0, "2.4: DIV 1/8 -> 1/4 in the first beat: the step on beat 2 plays");
    transport_req = 2; run_block();
}

/* SLOOP 2.4: steps of whole beats (DIV 1/2, 1BAR, 2BAR) and the dotted delay times */
static void t_longdiv(void)
{
    uint32_t i, k, n0, bpb = (uint32_t)((double)FS * 60.0 / 120.0 / CTL + 0.5);
    reset(120);
    TDRUM->p[P_SDIV] = 8;                             /* 2BAR: a step every 8 beats */
    TDRUM->p[P_SLEN] = 2;
    dstep_set(&TDRUM->dstep[0], 4, LV_NORM, 0);
    dstep_set(&TDRUM->dstep[1], 5, LV_NORM, 0);
    transport_req = 1;
    run_block();
    n0 = nhits;
    for (k = 0; k < 16u * bpb + 4u; k++) run_block();   /* 16 beats: steps at beats 0 and 8, step 0 again at 16 */
    {
        uint32_t a = 0, b = 0;
        for (i = 0; i < nhits; i++) a += hits[i].note == 36 || hits[i].note == 35, b += hits[i].note == 38 || hits[i].note == 37;
        check(nhits - n0 + 1u == 3u && clk_beat == 16u, "2.4: DIV 2BAR: one step every 8 beats (3 hits in 16 beats)");
        (void)a; (void)b;
    }
    transport_req = 2; run_block();
    reset(120);
    TDRUM->p[P_SDIV] = 6;                             /* 1/2: every 2 beats */
    TDRUM->p[P_SLEN] = 4;
    for (i = 0; i < 4u; i++) dstep_set(&TDRUM->dstep[i], 4, LV_NORM, 0);
    transport_req = 1;
    run_block();
    n0 = nhits;
    for (k = 0; k < 8u * bpb + 4u; k++) run_block();
    check(nhits - n0 + 1u == 5u, "2.4: DIV 1/2: one step every 2 beats (5 hits in 8 beats)");
    transport_req = 2; run_block();
    song.g[G_BPM] = 120;
    check(dly_samples(6) == (uint32_t)FS * 60u / 120u * 3u / 4u && dly_samples(7) == (uint32_t)FS * 60u / 120u * 3u / 8u,
          "2.4: delay TIME 1/8D = 3/4 beat, 1/16D = 3/8 beat");
    check(dly_samples(8) == (uint32_t)FS * 60u / 120u * 3u / 2u && GP[G_DTIME].max == 8 && !strcmp(GP[G_DTIME].names[8], "1/4D"),
          "2.6: delay TIME 1/4D = 1.5 beats, after 1/16D (older projects keep their value)");
    check(div_units(7) == BEAT_U * 4u && div_units(8) == BEAT_U * 8u && div_units(2) == BEAT_U / 4u,
          "2.4: div_units: 1BAR = 4 beats, 2BAR = 8, 1/16 = a quarter beat");
}

/* SLOOP 2.4: GLO > SYSTEM > MIDI = SEQ sends what the sequencer plays to MIDI OUT (note on, then off),
 * with the keys' channels; KEYS (the default) sends nothing of it; STOP ends every note sent */
static void mo_drain(uint32_t *on, uint32_t *off, uint32_t *onDrum, uint32_t *last_note)
{
    while (mo_r != mo_w) {
        uint32_t p = midi_out_q[mo_r % MQ], st = (p >> 8) & 0xF0u, ch = (p >> 8) & 15u;
        mo_r++;
        if (st == 0x90u && (p >> 24)) { (*on)++; if (ch == 9u) (*onDrum)++; else if (ch == 0u) *last_note = (p >> 16) & 127u; }
        else if (st == 0x80u || st == 0x90u) (*off)++;
    }
}
/* 2.6: GLO > MIDI: each synth track's own channel (in and out), and OTHER: SEL / OFF */
static int sounding(const track_t *t, uint32_t note)
{
    uint32_t v;
    for (v = 0; v < NVOICE; v++)
        if (t->v[v].active && t->v[v].gate && t->v[v].note == note)
            return 1;
    return 0;
}
static void cc_push(uint32_t ch, uint32_t cc, uint32_t v) { mclk_push(0x0Bu | (0xB0u | ch) << 8 | cc << 16 | v << 24); }
static void keys(uint32_t mask) { fm1_in.notes = mask; run_block(); run_block(); }
static void t_midichan(void)
{
    uint32_t i, ch = 99;
    uint8_t eng[NPART], amode[NPART];
    reset(120);
    mi_r = mi_w;
    uint8_t cfg = usb.config;
    for (i = 0; i < NPART; i++) eng[i] = trk[i].eng_req, amode[i] = (uint8_t)trk[i].p[P_AMODE];
    usb.config = 1;
    song.g[G_ROUTE] = 0;
    song.g[G_CH1] = 2, song.g[G_CH2] = 11, song.g[G_CH3] = 8, song.g[G_DRCH] = 10, song.g[G_CHOTH] = 0;   /* bass 2, pad 11, lead 8 */
    for (i = 0; i < NPART; i++) trk[i].p[P_AMODE] = 0, trk[i].eng_req = trk[i].engine = 0;
    song.sel = 2;
    mclk_push(0x09u | (0x90u | 1u) << 8 | 40u << 16 | 100u << 24);    /* ch 2 */
    mclk_push(0x09u | (0x90u | 10u) << 8 | 64u << 16 | 100u << 24);   /* ch 11 */
    mclk_push(0x09u | (0x90u | 7u) << 8 | 76u << 16 | 100u << 24);    /* ch 8 */
    mclk_push(0x09u | (0x90u | 0u) << 8 | 50u << 16 | 100u << 24);    /* ch 1: no track has it: the selected one (3) */
    run_block(); run_block();
    check(sounding(&trk[0], 40) && sounding(&trk[1], 64) && sounding(&trk[2], 76) && sounding(&trk[2], 50) &&
          !sounding(&trk[0], 50), "2.6: GLO > MIDI: ch 2, 11, 8 play tracks 1, 2, 3; ch 1 (nobody's): the selected track");
    cc_push(10, 5, 127); cc_push(1, 5, 0);
    run_block(); run_block();
    check(trk[1].p[P_GLIDE] == TP[P_GLIDE].max && trk[0].p[P_GLIDE] == TP[P_GLIDE].min, "2.6: a CC follows the track's channel too (CC 5 GLIDE)");
    for (i = 0; i < 128u; i++) {
        mclk_push(0x08u | (0x80u | 1u) << 8 | i << 16); mclk_push(0x08u | (0x80u | 10u) << 8 | i << 16);
        mclk_push(0x08u | (0x80u | 7u) << 8 | i << 16); mclk_push(0x08u | (0x80u | 0u) << 8 | i << 16);
        if (i % 16u == 15u) run_block();
    }
    run_block(); run_block();
    check(!sounding(&trk[0], 40) && !sounding(&trk[1], 64) && !sounding(&trk[2], 76) && !sounding(&trk[2], 50), "2.6: their note-offs end them");
    song.g[G_CHOTH] = 1;                                  /* OTHER = OFF */
    mclk_push(0x09u | (0x90u | 3u) << 8 | 55u << 16 | 100u << 24);    /* ch 4: nobody's */
    cc_push(3, 5, 127);
    run_block(); run_block();
    check(!sounding(&trk[0], 55) && !sounding(&trk[1], 55) && !sounding(&trk[2], 55) && trk[2].p[P_GLIDE] != TP[P_GLIDE].max,
          "2.6: OTHER = OFF: a channel no track has plays nothing (notes and CCs)");
    mo_r = mo_w;
    song.sel = 1;
    keys(1u << 7); keys(0);                               /* a key on track 2: MIDI out on its channel, 11 */
    while (mo_r != mo_w) {
        uint32_t p = midi_out_q[mo_r % MQ];
        mo_r++;
        if (((p >> 8) & 0xF0u) == 0x90u && (p >> 24)) ch = (p >> 8) & 15u;
    }
    check(ch == 10u, "2.6: MIDI out: a track's keys on its own channel (track 2 on 11)");
    song.g[G_CH1] = 1, song.g[G_CH2] = 2, song.g[G_CH3] = 3, song.g[G_CHOTH] = 0;
    song.sel = 0;
    mi_r = mi_w;
    usb.config = cfg;
    for (i = 0; i < NPART; i++) {                         /* (as it was for the tests after: engines, no voice) */
        uint32_t v;
        trk[i].eng_req = trk[i].engine = eng[i], trk[i].p[P_AMODE] = amode[i];
        for (v = 0; v < NVOICE; v++) trk[i].v[v].active = trk[i].v[v].gate = 0, trk[i].v[v].stage = 0;
    }
}
/* 2.6: the sustain pedal (CC 64): a note let go keeps sounding while it is down, ends when it is up; a note played
 * again meanwhile is held by its key */
static void t_sustain(void)
{
    uint8_t eng0 = trk[0].eng_req;
    uint32_t v;
    reset(120);
    mi_r = mi_w;
    song.g[G_ROUTE] = 0, song.g[G_CH1] = 1, song.g[G_CHOTH] = 0;
    trk[0].p[P_AMODE] = 0, trk[0].eng_req = trk[0].engine = 0, trk[0].p[P_REL] = 0;
    mclk_push(0x09u | 0x90u << 8 | 60u << 16 | 100u << 24);
    mclk_push(0x0Bu | 0xB0u << 8 | 64u << 16 | 127u << 24);   /* pedal down */
    mclk_push(0x08u | 0x80u << 8 | 60u << 16);
    run_block(); run_block(); run_block();
    check(!sounding(&trk[0], 60), "4.1: CC 64 is OP4 Osc Mode on a synth track now: no sustain pedal there");
    mclk_push(0x09u | 0x90u << 8 | 64u << 16 | 100u << 24);   /* E4 held through the pedal's release */
    run_block();
    mclk_push(0x0Bu | 0xB0u << 8 | 64u << 16 | 0u << 24);     /* pedal up */
    for (v = 0; v < 30u; v++) run_block();
    check(!sounding(&trk[0], 60) && sounding(&trk[0], 64), "2.6: sustain up: the waiting note ends, the one still held by its key stays");
    mclk_push(0x08u | 0x80u << 8 | 64u << 16);
    for (v = 0; v < 30u; v++) run_block();
    check(!sounding(&trk[0], 64), "2.6: (and ends with its key)");
    trk[0].eng_req = trk[0].engine = eng0;
    for (v = 0; v < NVOICE; v++) trk[0].v[v].active = trk[0].v[v].gate = 0, trk[0].v[v].stage = 0;
    mi_r = mi_w;
}
/* 2.6 review: the pedal and the wheel follow the track they acted on; OTHER = OFF ignores an unknown note-off; a
 * channel changed with a note held does not leave it stuck; a note played again under the pedal keeps the arp's count */
static void mf_on(uint32_t ch, uint32_t n) { mclk_push(0x09u | (0x90u | ch) << 8 | n << 16 | 100u << 24); }
static void mf_off(uint32_t ch, uint32_t n) { mclk_push(0x08u | (0x80u | ch) << 8 | n << 16); }
static void mf_cc(uint32_t ch, uint32_t c, uint32_t v) { mclk_push(0x0Bu | (0xB0u | ch) << 8 | c << 16 | v << 24); }
static void mf_prep(void)
{
    uint32_t i, v;
    reset(120);
    mi_r = mi_w;
    song.g[G_ROUTE] = 0, song.g[G_CH1] = 1, song.g[G_CH2] = 2, song.g[G_CH3] = 3, song.g[G_CHOTH] = 0, song.g[G_DRCH] = 10;
    for (i = 0; i < NPART; i++) {
        trk[i].p[P_AMODE] = 0, trk[i].p[P_AHOLD] = 0, trk[i].eng_req = trk[i].engine = 0, trk[i].p[P_REL] = 0;
        for (v = 0; v < NVOICE; v++)
            trk[i].v[v].active = trk[i].v[v].gate = 0, trk[i].v[v].stage = 0;
        sus_on[i] = 0, trk[i].bend = trk[i].bend_s = 0, trk[i].nheld = 0, trk[i].arp_phys = 0;
    }
    run_block();
}
static void t_midifix(void)
{
    uint8_t eng[NPART];
    uint32_t v, i;
    for (i = 0; i < NPART; i++)
        eng[i] = trk[i].eng_req;
    mf_prep();                                        /* the pedal on a SEL channel, the track changed while it is down */
    song.sel = 0;
    mf_on(4, 60), mf_cc(4, 64, 127), mf_off(4, 60);
    run_block(), run_block();
    song.sel = 1;
    mf_cc(4, 64, 0);
    for (v = 0; v < 30u; v++) run_block();
    check(!sounding(&trk[0], 60) && !sus_on[0], "2.6: pedal up after another track was selected: the first one's note ends");
    mf_on(0, 67), run_block(), mf_off(0, 67);
    for (v = 0; v < 30u; v++) run_block();
    check(!sounding(&trk[0], 67), "2.6:   and its later notes end with their keys (no pedal stuck)");
    mf_prep();                                        /* the wheel */
    song.sel = 0;
    mclk_push(0x0Eu | (0xE0u | 4u) << 8 | 127u << 16 | 127u << 24);
    run_block();
    song.sel = 1;
    mclk_push(0x0Eu | (0xE0u | 4u) << 8 | 0u << 16 | 64u << 24);
    run_block();
    check(trk[0].bend == 0 && trk[1].bend == 0, "2.6: the wheel let go after another track was selected: the first one back too");
    mclk_push(0x0Eu | (0xE0u | 0u) << 8 | 127u << 16 | 127u << 24);
    run_block();
    panic_req = 15, run_block();
    check(trk[0].bend == 0, "2.6: a project / NEW (every track released): the bends back to the middle");
    mf_prep();                                        /* OTHER = OFF */
    song.sel = 0, song.g[G_CHOTH] = 1;
    mf_on(0, 62), run_block(), run_block();
    mf_off(6, 62), run_block(), run_block();
    check(sounding(&trk[0], 62), "2.6: OTHER = OFF: a note-off on an ignored channel does not end the selected track's note");
    mf_off(0, 62);
    for (v = 0; v < 30u; v++) run_block();
    mf_prep();                                        /* re-struck under the pedal, ARP HOLD */
    song.sel = 0;
    trk[0].p[P_AMODE] = 1, trk[0].p[P_AHOLD] = 1;
    mf_cc(0, 64, 127), mf_on(0, 60), mf_off(0, 60), mf_on(0, 60), mf_off(0, 60), mf_cc(0, 64, 0);
    run_block(), run_block();
    check(trk[0].arp_phys == 0u, "2.6: a note played again under the pedal: no key left counted by the arp");
    trk[0].p[P_AMODE] = 0, trk[0].p[P_AHOLD] = 0, trk[0].nheld = 0;
    mf_prep();                                        /* Program Change (after Felucca 1.5.1): the track the channel plays */
    midi_pc[0] = midi_pc[1] = midi_pc[2] = midi_pc[3] = 0;
    mclk_push(0x0Cu | (0xC0u | 1u) << 8 | 5u << 16), mclk_push(0x0Cu | (0xC0u | 9u) << 8 | 2u << 16);
    run_block();
    {
        int ok = midi_pc[1] == 6u && midi_pc[3] == 3u && !midi_pc[0] && !midi_pc[2];
        midi_pc[1] = midi_pc[3] = 0;
        song.g[G_CHOTH] = 1, run_block();
        mclk_push(0x0Cu | (0xC0u | 6u) << 8 | 7u << 16);   /* ch 7: nobody's, OTHER = OFF */
        song.g[G_ROUTE] = 1;                              /* (and IN = CLOCK: none) */
        mclk_push(0x0Cu | (0xC0u | 0u) << 8 | 7u << 16);
        run_block();
        ok &= !midi_pc[0] && !midi_pc[1] && !midi_pc[2] && !midi_pc[3];
        song.g[G_ROUTE] = 0, song.g[G_CHOTH] = 0;
        check(ok, "2.6: MIDI Program Change: queued for the track its channel plays (ch 2: track 2, ch 10: drums); "
                  "OTHER = OFF / IN = CLOCK: none");
    }
    mf_prep();                                        /* a channel changed with a note held */
    song.sel = 0;
    mf_on(1, 70), run_block(), run_block();
    song.g[G_CH2] = 5;
    mf_off(1, 70);
    for (v = 0; v < 30u; v++) run_block();
    check(!sounding(&trk[1], 70), "2.6: GLO > MIDI CH2 changed with a note held on it: the note does not stay stuck");
    song.g[G_CH2] = 2;
    for (i = 0; i < NPART; i++) {
        trk[i].eng_req = trk[i].engine = eng[i];
        for (v = 0; v < NVOICE; v++)
            trk[i].v[v].active = trk[i].v[v].gate = 0, trk[i].v[v].stage = 0;
    }
    run_block();
    mi_r = mi_w;
}
/* 2.6: a track's MIDI OFF is off out too; a channel changed while the sequencer's note is on: its note-off goes out on
 * the channel the note went out on (no note left hanging on the other synth) */
static void t_midiout_ch(void)
{
    uint32_t k, i, ons = 0, chon = 0xFF, choff = 0xFF, bpb = (uint32_t)((double)FS * 60.0 / 120.0 / CTL + 0.5);
    reset(120);
    usb.config = 1;
    mo_r = mo_w;
    song.g[G_MIDI] = 1, song.g[G_CH1] = 0;
    trk[0].p[P_SDIV] = 0, trk[0].p[P_SLEN] = 4, trk[0].p[P_SGATE] = 120;
    for (i = 0; i < 4u; i++) { trk[0].step[i].time = ST_NOTE; trk[0].step[i].n = 1; trk[0].step[i].note[0] = 60; trk[0].step[i].vel = 100; }
    TDRUM->p[P_SLEN] = 4;
    transport_req = 1;
    for (k = 0; k < 2u * bpb; k++) run_block();
    while (mo_r != mo_w) {
        uint32_t p = midi_out_q[mo_r % MQ];
        mo_r++;
        ons += ((p >> 8) & 0xF0u) == 0x90u && ((p >> 8) & 15u) != 9u;
    }
    check(ons == 0u, "2.6: GLO > MIDI CH1 = OFF: track 1 sends nothing to MIDI OUT either");
    song.g[G_CH1] = 5;
    for (k = 0; k < bpb && chon == 0xFF; k++) {
        run_block();
        while (mo_r != mo_w) {
            uint32_t p = midi_out_q[mo_r % MQ];
            mo_r++;
            if (((p >> 8) & 0xF0u) == 0x90u && (p >> 24) && ((p >> 8) & 15u) != 9u)
                chon = (p >> 8) & 15u;
        }
    }
    song.g[G_CH1] = 6;                                /* the note still on */
    for (k = 0; k < 4u; k++) {
        run_block();
        while (mo_r != mo_w) {
            uint32_t p = midi_out_q[mo_r % MQ];
            mo_r++;
            if ((((p >> 8) & 0xF0u) == 0x80u || (((p >> 8) & 0xF0u) == 0x90u && !(p >> 24))) && ((p >> 16) & 127u) == 60u)
                choff = (p >> 8) & 15u;
        }
    }
    check(chon == 4u && choff == 4u, "2.6: CH1 changed with a sequencer note on: its note-off on the channel it went out on (5)");
    transport_req = 2, run_block();
    song.g[G_CH1] = 1, song.g[G_MIDI] = 0;
    usb.config = 0;
    mo_r = mo_w;
    for (k = 0; k < (uint32_t)(FS / CTL); k++)
        run_block();
}
static void t_midiout(void)
{
    uint32_t k, i, on = 0, off = 0, od = 0, ln = 0, bpb = (uint32_t)((double)FS * 60.0 / 120.0 / CTL + 0.5);
    reset(120);
    usb.config = 1;
    mo_r = mo_w;
    song.g[G_MIDI] = 0;
    trk[0].p[P_SDIV] = 0;                             /* 1/4: a note a beat */
    trk[0].p[P_SLEN] = 4;
    for (i = 0; i < 4u; i++) { trk[0].step[i].time = ST_NOTE; trk[0].step[i].n = 1; trk[0].step[i].note[0] = 60 + i; trk[0].step[i].vel = 100; }
    TDRUM->p[P_SDIV] = 0;
    TDRUM->p[P_SLEN] = 4;
    dstep_set(&TDRUM->dstep[0], 4, LV_NORM, 0);
    transport_req = 1;
    for (k = 0; k < 4u * bpb + 2u; k++) run_block();
    mo_drain(&on, &off, &od, &ln);
    check(on == 0 && off == 0, "2.4: MIDI = KEYS: the sequencer sends nothing to MIDI OUT");
    song.g[G_MIDI] = 1;
    for (k = 0; k < 4u * bpb; k++) run_block();
    mo_drain(&on, &off, &od, &ln);
    if (!(on >= 4u && od >= 1u && ln >= 60u && ln < 64u)) printf("seq2:   on %u off %u drum %u last %u\n", on, off, od, ln);
    check(on >= 4u && od >= 1u && ln >= 60u && ln < 64u, "2.4: MIDI = SEQ: the steps go out on their track's channel (drums on 10)");
    transport_req = 2; run_block();
    mo_drain(&on, &off, &od, &ln);
    check(on == off, "2.4: MIDI = SEQ: every note sent on was ended (STOP ends the rest)");
    song.g[G_MIDI] = 0;
    usb.config = 0;
    for (k = 0; k < (uint32_t)(FS / CTL); k++)
        run_block();                                  /* (the released voices die down for the next test) */
}


/* sloopDX 3.3: the MIDI OUT of an OMNI chord step (each note on its own, ended once), and MIDI IN = CLOCK: notes
 * from a computer play nothing */
static void t_midi33(void)
{
    uint32_t k, i, on = 0, off = 0, v, bpb = (uint32_t)((double)FS * 60.0 / 120.0 / CTL + 0.5);
    reset(120);
    usb.config = 1;
    mo_r = mo_w;
    song.g[G_MIDI] = 1;
    trk[1].p[P_AMODE] = AM_OMNI;
    trk[1].p[P_SDIV] = 0;
    trk[1].p[P_SLEN] = 2;
    trk[1].step[0].time = ST_NOTE; trk[1].step[0].n = 3;
    trk[1].step[0].note[0] = 57; trk[1].step[0].note[1] = 60; trk[1].step[0].note[2] = 64; trk[1].step[0].vel = 100;
    transport_req = 1;
    for (k = 0; k < 2u * bpb; k++) run_block();
    transport_req = 2; run_block();
    while (mo_r != mo_w) {
        uint32_t p = midi_out_q[mo_r % MQ], st = (p >> 8) & 0xF0u, ch = (p >> 8) & 15u;
        mo_r++;
        if (st == 0x90u && (p >> 24) && ch == 1u) on++;
        else if ((st == 0x80u || st == 0x90u) && ch == 1u) off++;
    }
    if (on != 3u || off != 3u) printf("seq2:   omni out: on %u off %u\n", on, off);
    check(on == 3u && off == 3u, "3.3: MIDI = SEQ: an OMNI chord step goes out as its 3 notes on track 2's channel, ended once");
    song.g[G_MIDI] = 0;
    trk[1].p[P_AMODE] = 0;
    reset(120);                                       /* IN = CLOCK */
    song.g[G_ROUTE] = 1;
    mi_r = mi_w;
    midi_in_q[mi_w % MQ] = 0x09u | 0x90u << 8 | 60u << 16 | 100u << 24; mi_w++;
    run_block(); run_block();
    for (v = i = 0; i < NVOICE; i++) v += trk[0].v[i].active && trk[0].v[i].gate && trk[0].v[i].note == 60u;
    check(v == 0u, "3.3: MIDI IN = CLOCK: a note from the computer plays nothing");
    song.g[G_ROUTE] = 0;
    midi_in_q[mi_w % MQ] = 0x09u | 0x90u << 8 | 60u << 16 | 100u << 24; mi_w++;
    run_block(); run_block();
    for (v = i = 0; i < NVOICE; i++) v += trk[0].v[i].active && trk[0].v[i].gate && trk[0].v[i].note == 60u;
    check(v > 0u, "3.3: MIDI IN = NOTES: the same note plays");
    midi_in_q[mi_w % MQ] = 0x08u | 0x80u << 8 | 60u << 16; mi_w++;
    usb.config = 0;
    for (k = 0; k < (uint32_t)(FS / CTL); k++) run_block();
}

/* sloopDX 3.4 (from SLOOP 2.5): MIDI CCs set the track the channel plays (DX7: 74 CUT, 73 ATK ...; the drum
 * channel: 7 / 94 GLO > DRUMS LVL / DLY), none with IN = CLOCK; the drums' delay send; the click with the drum
 * track muted */
static void cc_in(uint32_t ch, uint32_t cc, uint32_t v)
{
    midi_in_q[mi_w % MQ] = 0x0Bu | (0xB0u | ch) << 8 | cc << 16 | v << 24;
    mi_w++;
}
static int64_t blk_energy(const int32_t *b)          /* (of the change from sample to sample: no DC) */
{
    int64_t e = 0;
    uint32_t i;
    for (i = 1; i < CTL; i++)
        e += b[i] < b[i - 1] ? b[i - 1] - b[i] : b[i] - b[i - 1];
    return e;
}
/* 4.1: CC 10..99 on a synth track are the DX7 operators: CC = 10 + 15 x (OP - 1) + k (k: R1-4, L1-4, Output Level, Osc Mode,
 * Coarse, Fine, Detune, Key Velocity, Rate Scaling). A factory voice is copied into the first INIT VOICE slot first.
 * The unpacked voice: OP6 is block 0, OP1 block 5 (21 bytes each) */
static void t_opcc(void)
{
    uint32_t k;
    reset(120);
    mi_r = mi_w;
    trk[0].p[P_E0] = 0;                                   /* EPIANO 1, a factory voice */
    cc_in(0, 18, 127);                                    /* OP1 Output Level -> 99 */
    run_block();
    k = (uint32_t)trk[0].p[P_E0] - DX_NSYNTH;
    check(trk[0].p[P_E0] >= (int16_t)DX_NSYNTH && k < DX_NUSER && dx_user_param_get(k, 5u * 21u + 16u) == 99u,
          "4.1: CC 18 (OP1 Output Level): the factory voice is copied into a free slot, the value set");
    cc_in(0, 10, 0);                                      /* OP1 Rate 1 -> 0 */
    cc_in(0, 25, 127);                                    /* OP2 Rate 1 -> 99 */
    cc_in(0, 96, 127);                                    /* OP6 Fine -> 99 */
    cc_in(0, 37, 64);                                     /* OP2 Detune -> 7 (the middle) */
    cc_in(0, 49, 127);                                    /* OP3 Osc Mode -> 1 */
    cc_in(0, 99, 127);                                    /* OP6 Rate Scaling -> 7 */
    run_block();
    check((uint32_t)trk[0].p[P_E0] - DX_NSYNTH == k && dx_user_param_get(k, 5u * 21u + 0u) == 0u &&
          dx_user_param_get(k, 4u * 21u + 0u) == 99u && dx_user_param_get(k, 0u * 21u + 19u) == 99u &&
          dx_user_param_get(k, 4u * 21u + 20u) == 7u && dx_user_param_get(k, 3u * 21u + 17u) == 1u &&
          dx_user_param_get(k, 0u * 21u + 13u) == 7u && dx_user_param_get(k, 5u * 21u + 16u) == 99u,
          "4.1: OP1 R1, OP2 R1, OP6 Fine, OP2 Detune (64 = 7), OP3 Osc Mode, OP6 Rate Scaling: same slot");
    trk[0].p[P_E7] = 100;
    cc_in(0, 71, 0);                                      /* (was RESO) now OP5 Output Level... of the voice: not RESO */
    cc_in(0, 99, 0);
    run_block();
    check(trk[0].p[P_E7] == 100 && dx_user_param_get(k, 0u * 21u + 13u) == 0u,
          "4.1: CC 71 is no longer RESO; CC 99 back to 0");
}
static void t_cc34(void)
{
    uint32_t k, dch = (uint32_t)song.g[G_DRCH] - 1u;
    int64_t e0 = 0, e1 = 0;
    reset(120);
    mi_r = mi_w;
    dch = (uint32_t)song.g[G_DRCH] - 1u;
    cc_in(0, 5, 127); cc_in(1, 7, 0);
    cc_in(dch, 94, 127); cc_in(dch, 7, 64); cc_in(dch, 74, 0);
    run_block();
    check(trk[0].p[P_GLIDE] == TP[P_GLIDE].max && trk[1].p[P_LEVEL] == 0,
          "4.1: CCs on the synth channels: 5 GLIDE, 7 LEVEL (10 PAN, 71..75, 91, 93, 94 are the operators' now)");
    check(song.g[G_DRDLY] == 127 && song.g[G_DRLVL] == 64 && 1,
          "3.4: CCs on the drum channel: 94 DLY, 7 LVL (74: no CUT there, ignored)");
    song.g[G_ROUTE] = 1;
    cc_in(0, 5, 0);
    run_block();
    check(trk[0].p[P_GLIDE] == TP[P_GLIDE].max, "3.4: IN = CLOCK: CCs ignored");
    song.g[G_ROUTE] = 0;
    reset(120);                                       /* the drums' delay send */
    for (k = 0; k < 2u; k++) {
        int64_t *e = k ? &e1 : &e0;
        uint32_t b;
        song.g[G_DRDLY] = (int16_t)(k ? 127 : 0);
        drum_on(36, 120);
        for (b = 0; b < 40u; b++) {
            run_block();
            *e += blk_energy(acc[0].sd);
        }
        for (b = 0; b < (uint32_t)(FS / CTL); b++) run_block();
    }
    check(e0 == 0 && e1 > 0, "3.4: GLO > DRUMS DLY: the drum bus into the delay, nothing at 0");
    song.g[G_DRDLY] = 0;
    reset(120);                                       /* the click, the drum track muted */
    TDRUM->p[P_MUTE] = 1;
    for (k = 0; k < (uint32_t)(FS / CTL * 2); k++) run_block();     /* (4.1: let any earlier tail die away) */
    e0 = e1 = 0;
    drum_on(36, 120);
    for (k = 0; k < 40u; k++) { run_block(); e0 += blk_energy(acc[0].l); }
    for (k = 0; k < (uint32_t)(FS / CTL); k++) run_block();
    drum_on(77, 120);
    for (k = 0; k < 40u; k++) { run_block(); e1 += blk_energy(acc[0].l); }
    if (!(e0 < e1 / 100)) printf("seq2:   muted kick %lld, click %lld\n", (long long)e0, (long long)e1);
    check(e0 < e1 / 10 && e1 > 0, "3.4: the click sounds with the drum track muted (a lane does not)");
    TDRUM->p[P_MUTE] = 0;
    for (k = 0; k < (uint32_t)(FS / CTL); k++) run_block();
}


/* SLOOP 2.7: overdub over held notes. A pad (C E held over the bar: a NOTE, then TIE steps); a note recorded on step 9
 * over it, held one step: the pad goes on under it (not played again, not released), the new note ends after its step;
 * held longer, its end moves along; step entry still replaces */
static int32_t voice_of(const track_t *t, uint32_t note)   /* a gated voice playing note: its age, else -1 */
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (t->v[i].active && t->v[i].gate && t->v[i].note == note)
            return (int32_t)t->v[i].age;
    return -1;
}
static void t_overdub(void)
{
    track_t *t = &trk[0];
    double sb = (double)FS * 60.0 / 120.0 / 4.0 / CTL, lat = (double)REC_LAT / CTL;
    uint32_t into, slen, k, i;
    int32_t a60, a64;
    int ok;
    reset(120);
    t->p[P_VOICE] = V_POLY;
    t->p[P_SLEN] = 16;
    t->step[0].time = ST_NOTE; t->step[0].n = 2; t->step[0].note[0] = 60; t->step[0].note[1] = 64; t->step[0].vel = 100;
    for (i = 1; i < 16u; i++) { memset(&t->step[i], 0, sizeof t->step[i]); t->step[i].time = ST_TIE; }
    song.sel = 0;
    transport_req = 1;
    run_block();
    while (trk_grid(t, &into, &slen) != 4u) run_block();
    a60 = voice_of(t, 60), a64 = voice_of(t, 64);
    song.rec = 1u;                                   /* overdub, while it plays */
    while (trk_grid(t, &into, &slen) != 8u) run_block();
    for (k = 0; k < (uint32_t)(lat + 0.5) + 2u; k++) run_block();
    input_on(t, 67, 100);
    for (k = 0; k < (uint32_t)(sb * 0.6); k++) run_block();   /* held a little over half a step */
    input_off(t, 67);
    ok = t->step[8].time == ST_NOTE && t->step[8].n == 3 && t->step[8].note[2] == 67 && (t->step[8].flags & SF_CONT(0)) &&
         (t->step[8].flags & SF_CONT(1)) && !(t->step[8].flags & SF_CONT(2)) && step_pure_cont(&t->step[9]) &&
         t->step[9].n == 2 && t->step[10].time == ST_TIE;
    check(ok, "2.7 overdub: a note over the held pad: step 9 = pad going on + the new note; step 10 its end (pad only)");
    song.rec = 0;
    while (trk_grid(t, &into, &slen) != 15u) run_block();      /* this bar, on: the pad still the same voices */
    ok = a60 >= 0 && voice_of(t, 60) == a60 && voice_of(t, 64) == a64 && voice_of(t, 67) < 0;
    check(ok, "2.7 overdub: the pad goes on under it (same voices, never cut), the new note ended");
    while (trk_grid(t, &into, &slen) != 16u + 9u) run_block(); /* the next bar, on step 10 */
    for (k = 0; k < 8u; k++) run_block();
    {
        int32_t b60 = voice_of(t, 60);
        uint32_t has67 = voice_of(t, 67) >= 0;
        while (trk_grid(t, &into, &slen) != 16u + 11u) run_block();
        ok = b60 >= 0 && b60 != a60 && voice_of(t, 60) == b60 && voice_of(t, 64) >= 0 && !has67;
    }
    check(ok, "2.7 overdub, played back: the pad from step 1, held through steps 9-10, the new note only on 9");
    transport_req = 2; run_block();
    /* held three steps: its end moves to step 12 */
    reset(120);
    t->p[P_VOICE] = V_POLY;
    t->p[P_SLEN] = 16;
    t->step[0].time = ST_NOTE; t->step[0].n = 2; t->step[0].note[0] = 60; t->step[0].note[1] = 64; t->step[0].vel = 100;
    for (i = 1; i < 16u; i++) { memset(&t->step[i], 0, sizeof t->step[i]); t->step[i].time = ST_TIE; }
    song.sel = 0;
    song.rec = 1u;
    transport_req = 1;
    run_block();
    while (trk_grid(t, &into, &slen) != 8u) run_block();
    for (k = 0; k < (uint32_t)(lat + 0.5) + 2u; k++) run_block();
    input_on(t, 67, 100);
    while (trk_grid(t, &into, &slen) != 11u) run_block();
    for (k = 0; k < (uint32_t)(sb * 0.7); k++) run_block();
    input_off(t, 67);
    ok = t->step[8].n == 3 && t->step[9].time == ST_TIE && t->step[10].time == ST_TIE && t->step[11].time == ST_TIE &&
         step_pure_cont(&t->step[12]) && t->step[13].time == ST_TIE;
    if (!ok)
        for (i = 8; i < 14u; i++) printf("seq2:   step %u time %u n %u flags %02x\n", i + 1, t->step[i].time, t->step[i].n, t->step[i].flags);
    check(ok, "2.7 overdub: held four steps: ties to step 12, its end on 13");
    transport_req = 2; run_block();
    /* as on the FM-1: a note recorded held over 32 steps, then an overdub pass, another note pressed on step 17 */
    reset(120);
    t->p[P_VOICE] = V_POLY;
    t->p[P_SLEN] = 32;
    song.sel = 0;
    song.rec = 1u;
    transport_req = 1;
    run_block();
    input_on(t, 60, 100);
    while (trk_grid(t, &into, &slen) != 31u) run_block();
    for (k = 0; k < (uint32_t)(sb * 0.8); k++) run_block();
    input_off(t, 60);
    song.rec = 0;
    ok = t->step[0].time == ST_NOTE && t->step[0].note[0] == 60 && t->step[1].time == ST_TIE && t->step[31].time == ST_TIE;
    while (trk_grid(t, &into, &slen) != 32u + 16u) run_block();
    song.rec = 1u;
    for (k = 0; k < 20u; k++) run_block();
    a60 = voice_of(t, 60);
    input_on(t, 64, 100);
    for (k = 0; k < 3u; k++) run_block();
    ok &= a60 >= 0 && voice_of(t, 60) == a60 && voice_of(t, 64) >= 0;
    for (k = 0; k < (uint32_t)(sb * 0.5); k++) run_block();
    input_off(t, 64);
    for (k = 0; k < (uint32_t)(sb * 2.0); k++) run_block();
    ok &= voice_of(t, 60) == a60 && voice_of(t, 64) < 0 && t->step[16].n == 2 && (t->step[16].flags & SF_CONT(0)) &&
          step_pure_cont(&t->step[17]);
    song.rec = 0;
    while (trk_grid(t, &into, &slen) != 64u + 20u) run_block();   /* the next pass: still the one note, held */
    ok &= voice_of(t, 60) >= 0;
    check(ok, "2.7 overdub as played: 32 steps held, a note pressed on step 17 of the next pass: the held note goes on");
    transport_req = 2; run_block();
    song.rec = 0;
}


/* SLOOP 2.5: MIDI CCs set track parameters (Felucca 1.1.5's CC map), on the track the channel plays */
static void cc_push(uint32_t ch, uint32_t cc, uint32_t v);
/* SLOOP 2.6: GLO > SYSTEM > IN = CH n: only that channel, on the selected track; the others ignored */
static uint32_t gates(const track_t *t, uint32_t note)
{
    uint32_t k, on = 0;
    for (k = 0; k < NVOICE; k++) on += t->v[k].active && t->v[k].gate && (note > 127u || t->v[k].note == note);
    return on;
}
static void t_midich(void)
{
    reset(120);
    mi_r = mi_w;
    song.g[G_ROUTE] = 2 + 11;                           /* CH 12 */
    run_block();
    song.sel = 2;
    mclk_push(0x643C9009u);                             /* C4 on ch 1: ignored (track 1 would play it) */
    mclk_push(0x6440900Fu);                             /* E4 on ch 10 (the drums): ignored */
    mclk_push(0x64409509u);                             /* E4 on ch 6 (the selected track): ignored */
    run_block(); run_block();
    check(!gates(&trk[0], 200) && !gates(&trk[2], 200), "2.6: IN = CH 12: notes on other channels play nothing");
    mclk_push(0x64439B09u);                             /* G4 on ch 12 */
    run_block(); run_block();
    check(gates(&trk[2], 67) == 1u, "2.6: IN = CH 12: a note on channel 12 plays the selected track (3)");
    song.sel = 1;
    mclk_push(0x00438B08u);                             /* G4 off ch 12, track 2 selected meanwhile */
    mclk_push(0x00438008u);                             /* (G4 off on ch 1: ignored) */
    run_block(); run_block();
    check(!gates(&trk[2], 67) && !gates(&trk[1], 200), "2.6: its note-off ends it on track 3, where it started");
    {
        int16_t l1 = trk[1].p[P_LEVEL], l0 = trk[0].p[P_LEVEL];
        cc_push(11, 7, 20); cc_push(0, 7, 127);
        run_block(); run_block();
        check(trk[1].p[P_LEVEL] != l1 && trk[1].p[P_LEVEL] < 40 && trk[0].p[P_LEVEL] == l0,
              "2.6: a CC on channel 12 sets the selected track; on channel 1 it is ignored");
    }
    mclk_push(0x64409B09u);                             /* E4 on ch 12, held, then IN back to NOTES: released */
    run_block(); run_block();
    song.g[G_ROUTE] = 0;
    run_block(); run_block();
    check(!gates(&trk[1], 64), "2.6: IN changed while a note is held: it is released (no hanging note)");
    song.sel = 0;
    mi_r = mi_w;
}

/* 2.6 (after Felucca 1.5, #191): a latched arp follows TRN, ROOT, SCALE and QNT changed after its keys */
static uint32_t arp_notes(const track_t *t, uint32_t blocks)   /* the notes the arp played, as a bit set over 128 */
{
    static uint32_t m[4];
    uint32_t b, r = 0;
    m[0] = m[1] = m[2] = m[3] = 0;
    for (b = 0; b < blocks; b++) {
        run_block();
        if (t->arp_note) m[t->arp_note >> 5] |= 1u << (t->arp_note & 31u);
    }
    for (b = 0; b < 128u; b++) if ((m[b >> 5] >> (b & 31u)) & 1u) r = r * 131u + b + 1u;
    return r;
}
static uint32_t set2(uint32_t a, uint32_t b) { return (a + 1u) * 131u + b + 1u; }
static void t_arp191(void)
{
    track_t *t = &trk[0];
    reset(120);
    song.sel = 0; song.octave = 0;
    t->p[P_AMODE] = 1; t->p[P_AHOLD] = 1; t->p[P_AOCT] = 1; t->p[P_APROB] = 127;
    t->p[P_QUANT] = 0; t->p[P_TRANS] = 0; t->p[P_ROOT] = 0; t->p[P_SCALE] = 1; t->p[P_CHORD] = 0;
    keys(1u << 7 | 1u << 11); keys(0);                  /* C4 and E4, latched */
    check(arp_notes(t, 1500) == set2(60, 64), "#191: the latched arp plays its keys (C4 E4)");
    t->p[P_TRANS] = 2;
    arp_notes(t, 700);
    check(arp_notes(t, 1500) == set2(62, 66), "#191: TRN +2 after the keys: the latched arp follows (D4 F#4)");
    t->p[P_TRANS] = 0; t->p[P_QUANT] = 2; t->p[P_SCALE] = 2;   /* WHITE, minor: E4 is the 3rd degree, Eb4 */
    arp_notes(t, 700);
    check(arp_notes(t, 1500) == set2(60, 63), "#191: QNT WHITE, SCALE MIN after the keys: C4 Eb4");
    t->p[P_ROOT] = 2;                                   /* D minor */
    arp_notes(t, 700);
    check(arp_notes(t, 1500) == set2(62, 65), "#191: ROOT D after the keys: D4 F4");
    mclk_push(0x64409009u); run_block();                /* a MIDI note: it plays as it came, whatever the scale */
    t->p[P_AHOLD] = 0; keys(0); run_block(); run_block();
    t->p[P_AMODE] = 0; t->nheld = 0; t->arp_phys = 0;
    t->p[P_QUANT] = 0; t->p[P_SCALE] = 0; t->p[P_ROOT] = 0;
    mi_r = mi_w;
}

int main(void)
{
    t_midich();
    t_arp191();
    t_overdub();
    t_midichan();
    t_sustain();
    t_midifix();
    t_midiout_ch();
    t_usbfull();
    t_recmode();
    t_mclk();
    t_shed();
    t_drift();
    t_burst();
    t_swing_odd();
    t_len_phase();
    t_ratchet();
    t_roll();
    t_erase_undo();
    t_levels();
    t_chords();
    t_mute();
    t_fixes24();
    t_longdiv();
    t_midiout();
    t_midi33();
    t_cc34();
    t_opcc();
    printf("seq2: %s\n", fails ? "FAILED" : "all checks ok");
    return fails;
}
