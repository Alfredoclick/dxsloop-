/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Keyboard and its layers, scale and chords, arpeggiator, note repeat, sequencer, recording and
 * transport. Runs in the audio ISR, once per CTL-sample block (events_block), and ends in
 * trk_note_on / trk_note_off / drum_on: engines never see where a note came from.
 *
 * Time: the transport clock of fx.c (clk_beat, clk_pos, in units of a sample at 1 BPM). Every track
 * reads its step from it (trk_grid): the step is the clock divided by the track's DIV, swung, modulo
 * its LEN. So the tracks, the click, the arp, the rolls and the song arranger never drift apart, a
 * tempo or DIV change plays at most one step a block, and a polymeter (any LEN) stays in phase.
 *
 * Four tracks: tracks 1..3 are synth parts (steps of up to 4 notes), track 4 the drum track (steps
 * of 16 lanes, one per white key). Each step note / lane has a level (ghost .. hard) and a ratchet
 * (x1..x4 hits in its step). The keys play the selected track; MIDI channels 1..3 play parts 1..3,
 * the DRUMS channel (GLO > DRUMS, default 10) the drum track, any other channel the selected track.
 *
 * Layers: a function button held turns the keys into something else (TE style: hold + touch):
 *   FX   punch-in effects (punch.c)       EDIT  erase that note / sound (while held, as it plays)
 *   ARP  note repeat (roll) at G_ROLL     SEQ   steps 1..16 (the UI: ui_layers.c)
 *   SEL  the key of the song (the UI)     GLO   mute / solo / tap tempo (the UI)
 * On the drum track OCT- / OCT+ held play (and record) ghost / hard hits. */
static const uint16_t SCALE_MASK[] = {
    0xFFF,                                   /* CHR */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),   /* MAJ */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),   /* MIN */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),   /* DOR */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),   /* MIX */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),                          /* PEN */
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),                         /* MPEN */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),   /* HARM */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),   /* PHRY */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11),   /* LYD */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10),   /* LOC */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),   /* MEL (ascending) */
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),              /* BLUES (minor) */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 8) | (1 << 10),              /* WHOLE */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 10), /* DIMHW */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 9) | (1 << 11), /* DIMWH */
};
#define NSCALES (sizeof SCALE_MASK / sizeof SCALE_MASK[0])
#define SEQ_NONE 0xFFFFFFFFu

#define KB_SILENT 255u
static uint32_t kb_prev;
/* per key: what its press started, so its release ends the same (whatever the layer or track is now) */
enum { KS_NONE, KS_NOTE, KS_DRUM, KS_ROLL, KS_ERASE, KS_FX, KS_UI, KS_STRUM };
static uint8_t kb_kind[27], kb_trk[27], kb_n[27], kb_nt[27][4], kb_root[27];   /* kb_root: the key's own note (#191) */
static int8_t kb_oct[27];                   /* the octave buttons when the key went down (arp_add: held_key, #191) */
static uint8_t last_note = 60;
static uint8_t pen_n = 1, pen_note[4] = {60};   /* the last chord / note played: the SEQ layer writes it */
static uint8_t pen_lane;                       /* the last drum lane played: the SEQ layer's lane */
/* SLOOP 4.0 (SLOOP 2.6): the MIDI keyboard's notes. mi_pen: the notes held on the selected track (its pen, as the keys: the notes
 * down together are a chord). step_in_q: every synth note-on / note-off from MIDI, note | on << 7 | track << 8, for the
 * STEP page's entry (ui_input.c midi_entry): the audio side writes, the UI reads; a full queue drops and says so */
static uint32_t mi_pen[4];
#define STEP_IN_Q 32u
static volatile uint16_t step_in_q[STEP_IN_Q];
static volatile uint32_t step_in_w;
static volatile uint8_t step_in_lost;
static uint32_t step_in_r;
static void step_in_push(uint32_t ev)
{
    if (step_in_w - step_in_r >= STEP_IN_Q) {
        step_in_lost = 1;
        return;
    }
    step_in_q[step_in_w % STEP_IN_Q] = (uint16_t)ev;
    step_in_w++;
}
static volatile uint8_t transport_req;   /* 1 start, 2 stop (from the UI), 3 start from a MIDI START (no count-in) */
static volatile uint8_t panic_req;       /* bit per track: release every sounding note (preset / engine change) */
static volatile uint8_t arp_keep_req;    /* (2.7) bit per track: a sound loaded under its running arp: the arp's notes stay */

static uint32_t trk_index(const track_t *t) { return (uint32_t)(t - trk); }

static uint32_t trk_midi_ch(uint32_t i)    /* MIDI channel 0..15 of track i (keys -> MIDI out); 2.6: GLO > MIDI */
{                                          /* (2.6: OFF is OFF, in and out: 0xFF, nothing sent) */
    int32_t c = i < NPART ? song.g[G_CH1 + i] : song.g[G_DRCH];
    return c >= 1 && c <= 16 ? (uint32_t)c - 1u : 0xFFu;
}
/* a note on (vel > 0) / off to MIDI OUT on channel ch (trk_midi_ch: 0xFF, the track's MIDI OFF: nothing) */
static void midi_out_note(uint32_t ch, uint32_t note, uint32_t vel)
{
    if (ch > 15u)
        return;
    midi_out_event(vel ? 0x09u | (0x90u | ch) << 8 | (note & 127u) << 16 | (vel & 127u) << 24
                       : 0x08u | (0x80u | ch) << 8 | (note & 127u) << 16);
}

/* MIDI OUT of what the sequencer, the arp and the rolls play (GLO > SYSTEM > MIDI = SEQ, SLOOP 2.4; the keys always
 * go out: key_down / key_up). Notes from a computer or the jack are never echoed (no MIDI loop). A set per track
 * of the notes sent on, so a note is ended once, and STOP or MIDI = KEYS end them all */
static uint32_t mo_set[NTRK][4];
static uint8_t mo_ch[NTRK];                /* (2.6) the channel the notes of mo_set went out on: their note-offs too,
                                            * whatever GLO > MIDI says now (no note left hanging on the other gear) */
static void seq_out_off(const track_t *t, uint32_t note)
{
    uint32_t i = trk_index(t) % NTRK;
    if (note > 127u || !(mo_set[i][note >> 5] & (1u << (note & 31u))))
        return;
    mo_set[i][note >> 5] &= ~(1u << (note & 31u));
    midi_out_note(mo_ch[i], note, 0);
}
static void seq_out_track_off(const track_t *t);
static uint8_t mo_any;                     /* something was sent on since the last check (events_block) */
static void seq_out_on(const track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t i = trk_index(t) % NTRK;
    if (!song.g[G_MIDI] || note > 127u)
        return;
    seq_out_off(t, note);                      /* played again while on: off first */
    if (trk_midi_ch(i) != mo_ch[i]) {          /* (its channel changed: what is on ends where it started) */
        seq_out_track_off(t);
        mo_ch[i] = (uint8_t)trk_midi_ch(i);
    }
    if (mo_ch[i] > 15u)
        return;                                /* (MIDI OFF: nothing goes out, nothing to end) */
    mo_set[i][note >> 5] |= 1u << (note & 31u);
    mo_any = 1;
    midi_out_note(mo_ch[i], note, (vel ? vel & 127u : 1u));
}
static int seq_out_is_on(const track_t *t, uint32_t note)
{
    return note < 128u && (mo_set[trk_index(t) % NTRK][note >> 5] & (1u << (note & 31u))) != 0u;
}
static void seq_out_track_off(const track_t *t)      /* every note of the track still on */
{
    uint32_t i = trk_index(t) % NTRK, w, b;
    for (w = 0; w < 4u; w++)
        for (b = 0; mo_set[i][w]; b++)
            if (mo_set[i][w] & (1u << b)) {
                mo_set[i][w] &= ~(1u << b);
                midi_out_note(mo_ch[i], (w * 32u + b), 0);
            }
}
static void seq_out_all_off(void)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        seq_out_track_off(&trk[i]);
}

static uint32_t scale_mask(const track_t *t)
{
    return SCALE_MASK[clamp(t->p[P_SCALE], 0, NSCALES - 1)];
}

/* ---------------------------------------------------------- layers --- */
enum { LY_PLAY, LY_FX, LY_ERASE, LY_ROLL, LY_STEP, LY_SCALE, LY_MIX, LY_SONG, LY_COUNT };
static uint32_t ly_bit[LY_COUNT];        /* the button (fm1_in.buttons bit) of each layer: the UI sets them */
static uint32_t dyn_bit[2];              /* OCT- / OCT+: ghost / hard on the drum track */
/* a layer locked open (its button held + HOME tapped: ui_input.c), LY_PLAY = none: the keys and knobs
 * stay in it with the button let go, as if it were held */
static volatile uint8_t ly_lock = LY_PLAY;
static uint32_t layer_buttons(void) { return fm1_in.buttons | (ly_lock != LY_PLAY ? ly_bit[ly_lock % LY_COUNT] : 0u); }
/* the layer the keys are in: the held function button (FX, EDIT, ARP, SEQ, SEL, GLO in that order),
 * else the locked one */
static uint32_t layer_now(void)
{
    uint32_t b = layer_buttons(), l;
    for (l = LY_FX; l < LY_COUNT; l++)
        if (b & ly_bit[l])
            return l;
    return LY_PLAY;
}
/* the keys of the layers the UI handles (steps, key, mix): key k down / up, in order */
#define LKQ 16u
static volatile uint16_t lk_q[LKQ];
static volatile uint32_t lk_w, lk_r;
static void lk_push(uint32_t layer, uint32_t k, uint32_t down)
{
    if (lk_w - lk_r < LKQ) {
        lk_q[lk_w % LKQ] = (uint16_t)(layer << 8 | down << 7 | k);
        RING_PUBLISH();
        lk_w++;
    }
}

/* ------------------------------------------------------------- keys --- */
/* the keys of track t play through its scale (kb_pitch): not the drum track (its own map) */
static int kb_scaled(const track_t *t)
{
    return !is_drum(t);
}
/* key k's note on a scaled track with the octave buttons at oct (KB_SILENT: none): QNT, ROOT, SCALE, TRN as they are
 * now. WHITE (and chord mode): the white keys walk the scale from C4 = the root, the black keys are silent; SNAP: every
 * key, rounded down into the scale. The keys (kb_map), and the notes the arp holds (arp_list, 2.6 after Felucca 1.5
 * #191: a latched chord follows TRN, ROOT, SCALE and QNT, in the octave its keys were pressed in) */
static uint32_t kb_pitch(const track_t *t, uint32_t k, int32_t oct)
{
    static const int8_t DEGREE[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    int32_t n = 53 + (int32_t)k;
    if (t->p[P_QUANT] == 1 && !t->p[P_CHORD]) {  /* SNAP: every key, rounded down to the scale (the old ON) */
        uint32_t mask = scale_mask(t), guard = 12;
        n += 12 * oct + t->p[P_TRANS];
        while (guard-- && !((mask >> (uint32_t)((n - t->p[P_ROOT] + 120) % 12)) & 1u))
            n--;
        return (uint32_t)clamp(n, 0, 127);
    }
    if (t->p[P_QUANT] == 2 || t->p[P_CHORD]) {   /* WHITE: white keys walk the scale, black keys are silent */
        uint32_t mask = t->p[P_CHORD] && !t->p[P_SCALE] ? SCALE_MASK[2] : scale_mask(t), i;
        int32_t count = 0, degree = DEGREE[n % 12], wo;
        if (degree < 0)
            return KB_SILENT;
        /* C4 is the root. Walk scale degrees on successive white keys, including
         * below C4; scales with 5, 6, 8 or 12 notes still have no duplicated degrees. */
        degree += (n / 12 - 5) * 7;
        for (i = 0; i < 12u; i++)
            count += (mask >> i) & 1u;
        wo = degree / count;
        degree %= count;
        if (degree < 0) {
            degree += count;
            wo--;
        }
        for (i = 0; i < 12u; i++)
            if ((mask >> i) & 1u) {
                if (!degree)
                    break;
                degree--;
            }
        n = 60 + t->p[P_ROOT] + 12 * wo + (int32_t)i;
    }
    return (uint32_t)clamp(n + 12 * oct + t->p[P_TRANS], 0, 127);
}
/* key k -> note on track t (KB_SILENT: none) */
static uint32_t kb_map(const track_t *t, uint32_t k)
{
    if (is_drum(t))
        return LANE_NOTE[lane_of_key(k)];
    return kb_pitch(t, k, song.octave);
}

/* chord mode (P_CHORD): the chord of the scale built on note n (in the scale; CHR: minor), into c[];
 * the notes it holds (<= 4, the most a step keeps) */
static const int8_t CHORD_DEG[6][4] = {
    {0, -1, -1, -1},                     /* OFF */
    {0, 2, 4, -1},                       /* TRIAD: 1 3 5 */
    {0, 2, 4, 6},                        /* 7TH: 1 3 5 7 */
    {0, 2, 6, 8},                        /* 9TH: 1 3 7 9 (the lo-fi / R&B voicing) */
    {0, 3, 4, -1},                       /* SUS4: 1 4 5 */
    {0, -1, -1, -1},                     /* POWER: 1 5 8 (semitones, below) */
};
static uint32_t chord_notes(const track_t *t, uint32_t n, uint8_t *c)
{
    uint32_t type = (uint32_t)clamp(t->p[P_CHORD], 0, 5), mask = t->p[P_SCALE] ? scale_mask(t) : SCALE_MASK[2];
    uint32_t k = 0, j;
    if (type == 5u) {
        static const uint8_t PW[3] = {0, 7, 12};
        for (j = 0; j < 3u; j++)
            if (n + PW[j] < 128u)
                c[k++] = (uint8_t)(n + PW[j]);
        return k;
    }
    for (j = 0; j < 4u && CHORD_DEG[type][j] >= 0; j++) {
        int32_t m = (int32_t)n, d = CHORD_DEG[type][j], guard = 48;
        while (d > 0 && guard--) {                       /* d scale degrees up */
            m++;
            if ((mask >> (uint32_t)((m - t->p[P_ROOT] + 120) % 12)) & 1u)
                d--;
        }
        if (m < 128)
            c[k++] = (uint8_t)m;
    }
    return k;
}

/* ------------------------------------------------------------- grid --- */
/* units an odd step starts late: the track's + the global SWING (MPC: 0 = 50 %, 100 = 75 %) */
static uint32_t swing_units(int32_t pct, uint32_t u)
{
    return (uint32_t)clamp(pct, 0, 100) * u / 200u;
}
/* a position on a grid of steps u units long, odd steps sw late: the step, units into it, its length */
static uint32_t grid_swing(uint32_t abs, uint32_t frac, uint32_t u, uint32_t sw, uint32_t *into, uint32_t *len)
{
    if (abs & 1u) {                                      /* an odd step: sw late */
        if (frac < sw) {
            abs--;
            frac += u;
            *len = u + sw;
        } else {
            frac -= sw;
            *len = u - sw;
        }
    } else {
        *len = u + sw;
    }
    *into = frac;
    return abs;
}
/* the clock on a grid of den steps a beat (the rolls: ROLL_DEN) */
static uint32_t grid_den(uint32_t den, uint32_t sw, uint32_t *into, uint32_t *len)
{
    uint32_t u = BEAT_U / den;
    return grid_swing(clk_beat * den + clk_pos / u, clk_pos % u, u, sw, into, len);
}
/* the clock on the grid of a division (N_SDIV: steps inside a beat, or of 2, 4, 8 whole beats; SLOOP 2.4) */
static uint32_t grid_at(uint32_t div, uint32_t sw, uint32_t *into, uint32_t *len)
{
    uint32_t m;
    if (div < NDIV_SHORT)
        return grid_den(DIV_DEN[div], sw, into, len);
    m = DIV_BEATS[(div - NDIV_SHORT) % 3u];              /* whole beats: the step from the beat count */
    return grid_swing(clk_beat / m, (clk_beat % m) * BEAT_U + clk_pos, BEAT_U * m, sw, into, len);
}
/* swing is for the straight grids inside a beat: off on the triplet grids (on 8T the odd steps would
 * change from one beat to the next, as on most machines) and on steps of whole beats (SLOOP 2.4) */
static uint32_t swings(uint32_t div) { return div < 4u; }
static uint32_t trk_div(const track_t *t) { return (uint32_t)t->p[P_SDIV] % NDIV_STEP; }
static uint32_t trk_grid(const track_t *t, uint32_t *into, uint32_t *len)
{
    uint32_t div = trk_div(t);
    return grid_at(div, swings(div) ? swing_units(t->p[P_SSWING] + song.g[G_SWING], div_units(div)) : 0u, into, len);
}
static uint32_t trk_len(const track_t *t) { return t->p[P_SLEN] > 0 ? (uint32_t)t->p[P_SLEN] : 1u; }

/* ------------------------------------------------------------- undo --- */
/* One step back (and forward again) for the pattern of one track: what it was before the last
 * recording pass, erase, step edit, tool or clear (a session: one mark). EDIT + OCT- / OCT+. */
static struct {
    uint8_t valid, undone, trk;
    int16_t len;
    uint32_t sess;
    step_t st[NSTEP];
} undo;
static uint32_t undo_sess = 1;           /* UI sessions (seq.c: recording passes use the track's pass) */
static void undo_mark(const track_t *t, uint32_t sess)
{
    uint32_t i = trk_index(t);
    if (undo.valid && !undo.undone && undo.trk == i && undo.sess == sess)
        return;                                          /* (this session is marked already) */
    memcpy(undo.st, t->step, sizeof undo.st);
    undo.len = t->p[P_SLEN];
    undo.trk = (uint8_t)i;
    undo.sess = sess;
    undo.valid = 1;
    undo.undone = 0;
}
#define UNDO_REC(t) (((t)->pass << 2) | 1u)      /* a recording pass of track t */
static uint32_t undo_erase_sess;

/* ------------------------------------------------------- recording --- */
/* key to ear, in samples: the key's debounce (~3 ms) and the audio out buffer (HALF_FRAMES to
 * 2 x HALF_FRAMES, ~9 ms on average). A note played in time with what the player hears reaches
 * the sequencer this much later than the sound it was played to: recording takes it back. */
#define REC_LAT 512u

/* the step a note played now goes into (as heard: REC_LAT earlier): the one playing, or the next one
 * when it is past the middle of the playing one; *later: it has not played yet (it must not sound twice) */
static uint32_t rec_target(const track_t *t, uint32_t *later)
{
    uint32_t into, slen, abs = trk_grid(t, &into, &slen), half = slen / 2u, lat = REC_LAT * (uint32_t)song.g[G_BPM];
    int32_t off = (int32_t)into - (int32_t)lat;     /* where it was heard, from the start of the playing step */
    if (into > half + (lat < half ? lat : half)) {  /* (32 bits: a step is 8 beats at most, 8 x FS x 60 x 64 < 2^31) */
        abs++;
        off -= (int32_t)slen;                       /* (before the next one: early) */
    }
    *later = abs != t->seq_abs;
    return abs;
}
static uint8_t rec_quant_off;            /* (SLOOP 2.7's REC QUANT OFF needs its per-step nudges: not in sloopDX; kept
                                          * in the settings word, bit 31, so a word passes through unchanged) */

/* note into synth step idx (overdub: a step that holds notes gets this one added, a chord of up to 4;
 * when full, the last note is replaced; MONO / LEGATO / UNISON parts keep one note per step) */
static void step_add(track_t *t, uint32_t idx, uint32_t note, uint32_t vel, uint32_t lvl, uint32_t rat)
{
    step_t *s = &t->step[idx];
    uint32_t k;
    if (s->time != ST_NOTE || !s->n || t->p[P_VOICE] != V_POLY) {
        s->n = 0;                                   /* a fresh step */
        s->flags = 0;
        s->vel = 0;
        s->lvl = 0;
        s->rat = 0;
    }
    for (k = 0; k < s->n && s->note[k] != note; k++)
        ;
    if (k == s->n) {
        if (s->n < 4u)
            s->n++;
        k = s->n - 1u;
        s->note[k] = (uint8_t)note;
    }
    s->lvl = (uint8_t)((s->lvl & ~(3u << (2u * k))) | (lvl & 3u) << (2u * k));
    s->rat = (uint8_t)((s->rat & ~(3u << (2u * k))) | (rat & 3u) << (2u * k));
    s->time = ST_NOTE;
    if (vel > s->vel)
        s->vel = (uint8_t)vel;
}

/* ---- SLOOP 2.7: overdub over held notes ----
 * A pattern holds its notes with TIE steps (the notes of the NOTE step before them go on). A note recorded into a TIE
 * used to make that step a NOTE of its own, which ends the held notes there. Now (POLY parts) the step becomes a NOTE
 * with the held notes marked as going on (SF_CONT: not played again, not released) and the new note, up to 4 (the
 * oldest held one makes room); the step after it, when a TIE, becomes the new note's end: a NOTE of the held notes
 * only, all going on (step_pure_cont). While the new note is held, rec_hold moves that end along. Step entry (SEQ +
 * key) still replaces what plays. */
static uint32_t chain_notes(const track_t *t, uint32_t idx, uint8_t *out)   /* what goes on into TIE step idx */
{
    uint32_t len = trk_len(t), k;
    for (k = 1; k < len; k++) {
        const step_t *s = &t->step[(idx + len - k) % len];
        if (s->time == ST_TIE)
            continue;
        if (s->time == ST_NOTE && s->n && s->n <= 4u) {
            memcpy(out, s->note, s->n);
            return s->n;
        }
        return 0;
    }
    return 0;
}
static int step_pure_cont(const step_t *s)      /* a NOTE whose notes all go on: the end of an overdubbed note */
{
    return s->time == ST_NOTE && s->n && s->n <= 4u && ((s->flags & SF_CONT_ALL) / SF_CONT0) == (1u << s->n) - 1u;
}
static void chain_end(track_t *t, uint32_t j, const uint8_t *keep, uint32_t n)   /* TIE j: only keep goes on */
{
    step_t *s = &t->step[j % NSTEP];
    uint32_t i;
    if (s->time != ST_TIE || !n)
        return;
    memset(s, 0, sizeof *s);
    for (i = 0; i < n && i < 4u; i++) {
        s->note[i] = keep[i];
        s->flags |= (uint8_t)SF_CONT(i);
    }
    s->n = (uint8_t)i;
    s->time = ST_NOTE;
}
static uint32_t cont_notes(const step_t *s, uint32_t except, uint8_t *out)   /* its notes going on, but except */
{
    uint32_t i, n = 0;
    for (i = 0; i < s->n && i < 4u; i++)
        if ((s->flags & SF_CONT(i)) && s->note[i] != except)
            out[n++] = s->note[i];
    return n;
}

/* live recording into a synth part: the nearest step (rec_target). Held on: each further step the
 * sequencer enters while the note is held becomes a TIE (rec_hold), up to the pattern length and
 * never over a step with notes (overdub keeps them); a release before the middle of the last one
 * puts that step back (rec_release), so a short note stays one step. A note recorded into another
 * step ends the hold before (the step model ties the notes of one step only). */
static void rec_note(track_t *t, uint32_t note, uint32_t vel, uint32_t rat, int hold)
{
    uint32_t len = trk_len(t), later, abs = rec_target(t, &later), idx = abs % len, k;
    step_t *st = &t->step[idx % NSTEP];
    uint8_t ch[4], keep[4];
    uint32_t nc, nk = 0;
    undo_mark(t, UNDO_REC(t));
    if (t->p[P_VOICE] == V_POLY && !rat && st->time == ST_TIE && (nc = chain_notes(t, idx, ch)) != 0) {
        for (k = 0; k < nc; k++)                    /* (2.7) over held notes: they go on, this one starts */
            if (ch[k] != note)
                keep[nk++] = ch[k];
        while (nk > 3u) {                           /* (four at most: the oldest one ends here) */
            for (k = 0; k + 1u < nk; k++)
                keep[k] = keep[k + 1u];
            nk--;
        }
        memset(st, 0, sizeof *st);
        for (k = 0; k < nk; k++) {
            st->note[k] = keep[k];
            st->flags |= (uint8_t)SF_CONT(k);
        }
        st->note[nk] = (uint8_t)note;
        st->n = (uint8_t)(nk + 1u);
        st->time = ST_NOTE;
        st->vel = (uint8_t)vel;
        st->lvl = (uint8_t)((vel_lvl(vel) & 3u) << (2u * nk));
        if ((idx + 1u) % len != idx)
            chain_end(t, (idx + 1u) % len, keep, nk);
    } else {
        step_add(t, idx, note, vel, vel_lvl(vel), rat);
        for (k = 0; k < st->n; k++)                 /* (2.7) played again: it starts here */
            if (st->note[k] == note)
                st->flags &= (uint8_t)~SF_CONT(k);
        if ((st->flags & SF_CONT_ALL) && (nk = cont_notes(st, note, keep)) != 0 && (idx + 1u) % len != idx)
            chain_end(t, (idx + 1u) % len, keep, nk);   /* (into an end: the held ones go on past it) */
    }
    if (hold)                                       /* (a note played: hold; the arp's and a roll's: not) */
    t->seq_active = 1;
    if (later) {                                    /* it sounds now: the step must not trigger it again */
        if (t->rskip_abs != abs)
            t->rskip_n = 0;
        t->rskip_abs = abs;
        if (t->rskip_n < 4u)
            t->rskip[t->rskip_n++] = (uint8_t)note;
    }
    if (!hold)
        return;
    if (!t->rh_n || t->rh_start != idx) {           /* a new hold (one in another step ends) */
        t->rh_n = 0;
        t->rh_start = (uint8_t)idx;
        t->rh_ties = 0;
    }
    for (k = 0; k < t->rh_n && t->rh_note[k] != note; k++)
        ;
    if (k == t->rh_n && t->rh_n < 4u)
        t->rh_note[t->rh_n++] = (uint8_t)note;      /* a chord: held until its last key is up */
}

/* live recording into the drum track: lane, level, ratchet */
static uint8_t roll_rec;                         /* (2.7) a roll's hit is being recorded (roll_hit) */
static void rec_hit(track_t *t, uint32_t lane, uint32_t lvl, uint32_t rat)
{
    uint32_t later, abs = rec_target(t, &later), idx = abs % trk_len(t);
    undo_mark(t, UNDO_REC(t));
    dstep_set(&t->dstep[idx], lane, lvl, rat);
    if (!roll_rec)                                  /* (a roll's hits stay on its grid) */
    t->seq_active = 1;
    if (later) {
        if (t->rskip_abs != abs)
            t->rskip_lanes = 0;
        t->rskip_abs = abs;
        t->rskip_lanes |= (uint16_t)(1u << lane);
    }
}

/* the sequencer enters step idx (before playing it): a recorded note still held ties into it */
static void rec_hold(track_t *t, uint32_t idx, uint32_t len, uint32_t abs)
{
    step_t *s;
    if (!t->rh_n)
        return;
    if (!((song.rec >> trk_index(t)) & 1u) || t->rh_ties + 1u >= len) {
        t->rh_n = 0;                                /* disarmed, or the whole pattern is this note */
        return;
    }
    if (idx == t->rh_start)
        return;                                     /* (recorded ahead into the step now starting) */
    s = &t->step[idx];
    if (step_pure_cont(s)) {                        /* (2.7) the end of a note over held ones: it goes on, the end moves */
        uint8_t keep[4];
        uint32_t nk = cont_notes(s, 0x100u, keep);
        t->rh_bak = *s;
        t->rh_last = (uint8_t)idx;
        t->rh_last_abs = abs;
        t->rh_ties++;
        memset(s, 0, sizeof *s);
        s->time = ST_TIE;
        if ((idx + 1u) % len != t->rh_start)
            chain_end(t, (idx + 1u) % len, keep, nk);
        return;
    }
    if (s->time == ST_NOTE && s->n) {
        t->rh_n = 0;                                /* a step with notes: the hold ends before it */
        return;
    }
    t->rh_bak = *s;
    t->rh_last = (uint8_t)idx;
    t->rh_last_abs = abs;
    t->rh_ties++;
    memset(s, 0, sizeof *s);
    s->time = ST_TIE;
}

/* a key of a recorded note is up: the hold ends with the last one */
static void rec_release(track_t *t, uint32_t note)
{
    uint32_t i, k = 0, into, slen, half, lat;
    for (i = 0; i < t->rh_n; i++)
        if (t->rh_note[i] != note)
            t->rh_note[k++] = t->rh_note[i];
    if (k == t->rh_n || (t->rh_n = (uint8_t)k))
        return;                                     /* not one of them, or others still held */
    if (!t->rh_ties || trk_grid(t, &into, &slen) != t->rh_last_abs)
        return;
    half = slen / 2u;
    lat = REC_LAT * (uint32_t)song.g[G_BPM];
    if (into <= half + (lat < half ? lat : half) && t->step[t->rh_last].time == ST_TIE)
        t->step[t->rh_last] = t->rh_bak;            /* released early in it: not held into this step */
}

/* LIVE recording, no click needed (REC: ui_input.c rec_toggle).
 *  playing: REC records the selected track at once (song.rec), quantised, overdub.
 *  stopped: REC arms (rec_wait). The first note played on the selected track:
 *   - a project with notes: starts the transport, that note is step 1, recording on.
 *     PLAY while armed starts the transport and the recording together.
 *   - an empty project: a FREE TAKE (ft_on). Play freely, as long as you like: no tempo, no
 *     grid. REC on the next downbeat closes the loop: its length sets the tempo (1, 2 or 4
 *     bars, the nearest the current tempo; within 3 % of it the tempo is kept), the notes are
 *     quantised to 1/16 into it with their lengths, and the loop plays on. PLAY drops the take.
 *  The REC screen (ui_studio.c) sets how (settings of the FM-1, panel.c lights_word):
 *   KNOB 1 MODE (an empty project): FREE (the free take above) or TEMPO (record at the tempo set,
 *          as in a project with notes);
 *   KNOB 3 START (TEMPO, or a project with notes): NOTE (the first note starts the loop, as above)
 *          or COUNT (PLAY clicks one bar, 4 beats, then the loop and the recording start; notes
 *          played meanwhile only sound). */
static volatile uint8_t rec_wait;             /* 1: armed, waits for a note */
static uint8_t rec_tempo;                     /* REC screen MODE: 0 FREE, 1 TEMPO (an empty project) */
static uint8_t rec_count;                     /* REC screen START: 0 NOTE, 1 COUNT (one bar of clicks) */
static volatile uint8_t ci_on;                /* the count-in runs (armed, COUNT, PLAY) */
static volatile uint8_t ci_beat;              /* its beats clicked so far - 1 (the UI shows 4 - ci_beat) */
static uint32_t ci_u;                         /* clock units since it started */
static volatile uint8_t rec_go;               /* recording just started (the UI says so) */
static void seq_start(void);
static void rec_begin(void)
{
    song.rec = (uint8_t)(1u << (song.sel % NTRK));
    rec_wait = 0;
    rec_go = 1;
}

#define FT_MAX 192u
#define FT_BLOCKS (24u * FS / CTL)            /* 24 s: 4 bars at 40 BPM, the longest loop */
#define FT_OPEN 0xFFFFu                       /* the key is still down */
typedef struct { uint16_t t, d; uint8_t note, vel; } ft_ev_t;   /* in blocks from the first note; drums: lane, level */
static ft_ev_t ft_ev[FT_MAX];
static uint32_t ft_n;
static volatile uint8_t ft_on;                /* a free take runs */
static uint8_t ft_trk;
static volatile uint32_t ft_t;                /* blocks since its first note */
static volatile uint32_t ft_btn_mask;         /* the REC button (the UI sets it): closes the take */
static volatile uint32_t ft_drop_mask;        /* the PLAY button: drops it */
static uint32_t ft_btn_prev;
static volatile uint8_t ft_bars;              /* the loop just closed: bars (the UI says so), 0xFF dropped */
static volatile uint32_t ft_close_ms;         /* when (the UI drops the press that closed it) */
static volatile uint8_t ft_closed;            /* (ft_close_ms is set) */

static int track_empty(const track_t *t)
{
    uint32_t k;
    for (k = 0; k < NSTEP; k++)
        if (is_drum(t) ? dstep_mask(&t->dstep[k]) != 0u : t->step[k].time == ST_NOTE && t->step[k].n)
            return 0;
    return 1;
}
static int project_empty(void)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        if (!track_empty(&trk[i]))
            return 0;
    return 1;
}
static void steps_clear(track_t *t)           /* an empty pattern (synth: REST steps, drums: no lane) */
{
    uint32_t k;
    memset(t->step, 0, sizeof t->step);
    if (!is_drum(t))
        for (k = 0; k < NSTEP; k++)
            t->step[k].time = ST_REST;
}

/* tempo x 10 of a loop of T blocks holding n bars of 4/4 */
static uint32_t ft_bpm10(uint32_t T, uint32_t n)
{
    uint32_t den = T * CTL;                       /* (n * 2400 * FS < 2^32 up to n = 4) */
    return den ? (n * 2400u * FS + den / 2u) / den : 0u;
}

/* the bars a loop of T blocks holds (1, 2 or 4: the tempo the nearest the current one, in
 * 40..240), 0 none; *bpm its tempo */
static uint32_t ft_fit(uint32_t T, uint32_t *bpm)
{
    static const uint8_t BARS[3] = {1, 2, 4};
    uint32_t i, best = 0, cur = (uint32_t)song.g[G_BPM] * 10u, err = 0xFFFFFFFFu;
    for (i = 0; i < 3u; i++) {
        uint32_t b = ft_bpm10(T, BARS[i]), e;
        if (b < 400u || b > 2400u)
            continue;
        e = b > cur ? b * 1000u / cur : cur * 1000u / b;   /* the ratio, x 1000 */
        if (e < err) {
            err = e;
            best = BARS[i];
            *bpm = (b + 5u) / 10u;
            if (e <= 1030u)
                *bpm = cur / 10u;                           /* played to the tempo set: keep it */
        }
    }
    return best;
}

static void ft_start(track_t *t)
{
    ft_on = 1;
    ft_trk = (uint8_t)trk_index(t);
    ft_t = 0;
    ft_n = 0;
    rec_wait = 0;
}

static void ft_note_on(uint32_t note, uint32_t vel)
{
    if (ft_n < FT_MAX && ft_t < FT_BLOCKS) {
        ft_ev[ft_n].t = (uint16_t)ft_t;
        ft_ev[ft_n].d = FT_OPEN;
        ft_ev[ft_n].note = (uint8_t)note;
        ft_ev[ft_n].vel = (uint8_t)vel;
        ft_n++;
    }
}

static void ft_note_off(uint32_t note)
{
    uint32_t i = ft_n;
    while (i--)
        if (ft_ev[i].note == note && ft_ev[i].d == FT_OPEN) {
            ft_ev[i].d = (uint16_t)(ft_t - ft_ev[i].t);
            return;
        }
}

/* REC on the downbeat: the loop is the time from the first note to now */
static void ft_close(void)
{
    track_t *t = &trk[ft_trk % NTRK];
    uint32_t T = ft_t, bars, bpm = 0, len, i, k;
    ft_on = 0;
    ft_close_ms = fm1_ms;
    ft_closed = 1;
    bars = ft_n && T >= FS / CTL / 2u ? ft_fit(T, &bpm) : 0u;
    if (!bars) {
        ft_bars = 0xFF;                               /* nothing played, or no tempo fits */
        return;
    }
    len = 16u * bars;
    undo_mark(t, (undo_sess += 4u) | 3u);             /* (undo: back to the empty project) */
    steps_clear(t);                                   /* (no tie left over from an old pattern) */
    for (i = 0; i < NTRK; i++) {                      /* the project is empty: one loop length */
        trk[i].p[P_SLEN] = (int16_t)len;
        trk[i].p[P_SDIV] = 2;                         /* 1/16 */
    }
    for (i = 0; i < ft_n; i++) {                      /* the notes, to the nearest step */
        uint32_t idx = (ft_ev[i].t * len * 2u + T) / (2u * T) % len;
        if (is_drum(t))
            dstep_set(&t->dstep[idx], ft_ev[i].note & 15u, ft_ev[i].vel & 3u, 0);
        else
            step_add(t, idx, ft_ev[i].note, ft_ev[i].vel, vel_lvl(ft_ev[i].vel), 0);
    }
    if (!is_drum(t))
        for (i = 0; i < ft_n; i++) {                  /* their lengths: TIE steps, up to the next note */
            uint32_t d = ft_ev[i].d == FT_OPEN || ft_ev[i].t + ft_ev[i].d > T ? T - ft_ev[i].t : ft_ev[i].d;
            uint32_t idx = (ft_ev[i].t * len * 2u + T) / (2u * T) % len;
            uint32_t steps = (d * len * 2u + T) / (2u * T);
            for (k = 1; k < steps && k < len; k++) {
                step_t *s = &t->step[(idx + k) % len];
                if (s->time == ST_NOTE && s->n)
                    break;
                memset(s, 0, sizeof *s);
                s->time = ST_TIE;
            }
        }
    t->seq_active = 1;
    song.g[G_BPM] = (int16_t)bpm;
    ft_bars = (uint8_t)bars;
#if FELUCCA_ARRANGER
    arrangement_enabled = 0;
#endif
    seq_start();                                      /* this is the downbeat: the loop plays */
}

/* the UI: a REC / PLAY press is this one's (it runs the take, or just closed it): not the UI's */
static int ft_owns_press(void)
{
    return ft_on || (ft_closed && (uint32_t)(fm1_ms - ft_close_ms) < 300u);
}

/* a free take, once per block: REC closes it, PLAY drops it (their press, timed here, not by the UI) */
static void ft_block(void)
{
    uint32_t b = fm1_in.buttons & (ft_btn_mask | ft_drop_mask), press = b & ~ft_btn_prev;
    ft_btn_prev = b;
    if (!ft_on)
        return;
    ft_t++;
    if (press & ft_drop_mask) {
        ft_on = 0;                                    /* PLAY: dropped, nothing changes */
        ft_bars = 0xFF;
        ft_close_ms = fm1_ms;
        ft_closed = 1;
    } else if ((press & ft_btn_mask) || ft_t >= FT_BLOCKS) {
        ft_close();
    }
}

/* ------------------------------------------------------------ erase --- */
/* EDIT + key: that note / sound goes from the selected pattern. Playing: from the step playing and
 * every step the sequencer enters while the key is held (MPC style); stopped: from every step. */
static uint8_t er_trk;
static uint16_t er_lanes;                     /* drums: lanes held */
/* the drum track's live mutes (SLOOP 2.6, after Felucca 1.4.1): EDIT + a black key turns the white keys from erasing
 * into muting (er_mute; back to erasing with a black key again; 2.7: it stays when EDIT is let go), and a white
 * key then mutes / unmutes its sound: the sequencer skips it (its ratchets too), the keys still play it. Performance
 * state: never saved, not undone; a project load or NEW clears it */
static volatile uint16_t drum_mute;
static volatile uint8_t er_mute;
static uint32_t er_notes[4];                  /* synth: notes held (bit per MIDI note) */
static volatile uint8_t er_flash;             /* something was erased (the UI flashes) */

static int er_has(uint32_t note) { return (er_notes[(note >> 5) & 3u] >> (note & 31u)) & 1u; }
static void erase_step(track_t *t, uint32_t idx)
{
    if (is_drum(t)) {
        dstep_t *s = &t->dstep[idx];
        uint32_t l, m = dstep_mask(s) & er_lanes;
        for (l = 0; m; l++, m >>= 1)
            if (m & 1u) {
                dstep_clr(s, l);
                er_flash = 1;
            }
    } else {
        step_t *s = &t->step[idx];
        uint32_t i, k = 0, lv = 0, rt = 0, cf = 0;
        if (s->time != ST_NOTE)
            return;
        for (i = 0; i < s->n; i++)
            if (!er_has(s->note[i])) {
                lv |= ((s->lvl >> (2u * i)) & 3u) << (2u * k);
                rt |= ((s->rat >> (2u * i)) & 3u) << (2u * k);
                cf |= (s->flags & SF_CONT(i)) ? SF_CONT(k) : 0u;   /* (2.7: a note going on stays so) */
                s->note[k++] = s->note[i];
            }
        if (k == s->n)
            return;
        er_flash = 1;
        s->n = (uint8_t)k;
        s->lvl = (uint8_t)lv;
        s->rat = (uint8_t)rt;
        s->flags = (uint8_t)((s->flags & ~SF_CONT_ALL) | cf);
        if (!k) {
            s->time = ST_REST;
            s->flags = 0;
            s->vel = 0;
        }
    }
}
static void erase_now(track_t *t)             /* a key just went down */
{
    uint32_t i, len = trk_len(t);
    undo_mark(t, undo_erase_sess);
    if (song.playing && t->seq_abs != SEQ_NONE) {
        erase_step(t, t->seq_idx % len);
    } else {
        for (i = 0; i < len; i++)
            erase_step(t, i);
    }
}
static int erasing(const track_t *t) { return trk_index(t) == er_trk && (er_lanes || er_notes[0] || er_notes[1] || er_notes[2] || er_notes[3]); }

/* ------------------------------------------------------------- arp --- */
/* 2.6 (after Felucca 1.5, #191): where each track's held[i] came from, so the arp maps it again as it plays (arp_list:
 * TRN, ROOT, SCALE, QNT as they are now). held_key: the key + 1 (bits 0..4) and the octave buttons then + 3 (bits
 * 5..7); 0: not a key of the track's scale (MIDI IN, the drum track, the GM kit, SLICE), held[i] plays as it is.
 * held_dt: held[i] above the key's own note (a chord's other notes). in_key: the key (+ 1) whose notes go into
 * input_on now (key_down, chord_revoice), 0: none */
static uint8_t held_key[NTRK][16];
static int8_t held_dt[NTRK][16];
static uint8_t in_key;
static void arp_add(track_t *t, uint32_t note)
{
    uint32_t i;
    if (t->p[P_AHOLD] && t->arp_phys == 0u)
        t->nheld = 0;                               /* new chord replaces the latched one */
    t->arp_phys++;                                  /* every key-down: arp_remove counts every key-up */
    for (i = 0; i < t->nheld; i++)
        if (t->held[i] == note)
            return;                                 /* repeated note-on: not a new note */
    if (t->nheld < 16u) {
        uint32_t k = in_key - 1u, ti = trk_index(t);
        held_key[ti][t->nheld] = in_key && kb_scaled(t) ? (uint8_t)(in_key | (uint32_t)(kb_oct[k % 27u] + 3) << 5) : 0u;   /* (the
                                                       * key's octave: a CHORD+ note added after OCT moved, too) */
        held_dt[ti][t->nheld] = (int8_t)(held_key[ti][t->nheld] ? (int32_t)note - kb_root[k % 27u] : 0);
        t->held[t->nheld++] = (uint8_t)note;
    }
    if (t->nheld == 1u) {
        t->arp_new = 1;                             /* the first note: now (or on the grid just ahead) */
        t->arp_idx = 0xFFFFFFFFu;
    }
}

static void arp_remove(track_t *t, uint32_t note)
{
    uint32_t i, k = 0;
    if (t->arp_phys)
        t->arp_phys--;
    if (t->p[P_AHOLD])
        return;
    for (i = 0; i < t->nheld; i++)
        if (t->held[i] != note) {
            held_key[trk_index(t)][k] = held_key[trk_index(t)][i];
            held_dt[trk_index(t)][k] = held_dt[trk_index(t)][i];
            t->held[k++] = t->held[i];
        }
    t->nheld = (uint8_t)k;
}

/* the ARP's modes (P_AMODE). Append-only: a stored value keeps its meaning. 2.7, after Felucca 1.4 / 1.5.1 (its
 * Discussion #95), over the note list (the held notes low to high, or as played with ARP 2 ORD PLAY, across OCT
 * octaves), C E G held: DNUP G E C E; UP+8 C E G C' (the first note again an octave above the top); CONV C G E
 * (outside in); DIVG E G C (inside out); PINKY C G E G (the top note between the others); THUMB C E C G (the bottom
 * one); WALK a random step up or down the list (bouncing off its ends); REPEAT the last note played, again and again;
 * CHORD all the held notes together (the first 4), an octave higher each step through OCT. ORD (Felucca 1.4) always
 * plays the notes in the order they were pressed (up to 2.6 it played as UP unless ARP 2 ORD was PLAY) */
enum { AM_OFF, AM_UP, AM_DN, AM_UPDN, AM_RND, AM_ORD,   /* (6 OMNI, 7 FLW: core.h) SLOOP 2.7's modes after them */
       AM_REPEAT = AM_FLW + 1, AM_DNUP, AM_UP8, AM_CONV, AM_DIVG, AM_PINKY, AM_THUMB, AM_WALK, AM_CHORD };

/* the arp's note list (its length returned, 0: nothing to play; up to 64, + 1 for UP+8): the held notes as they play
 * now (#191), each once (*cnt of them), sorted or as played, over OCT octaves */
static uint32_t arp_list(const track_t *t, uint32_t *list, uint32_t *cnt)
{
    uint32_t n = 0, len = 0, i, j, o, ti = trk_index(t), last = 0, m = (uint32_t)t->p[P_AMODE];
    for (i = 0; i < t->nheld; i++) {               /* the notes as they play now (#191), each once */
        uint32_t x = t->held[i], s = held_key[ti][i];
        if (s) {                                    /* a key: through the scale as it is now */
            x = kb_pitch(t, (s & 31u) - 1u, (int32_t)(s >> 5) - 3);
            if (x == KB_SILENT)
                continue;                           /* (silent now: a black key under WHITE) */
            x = (uint32_t)clamp((int32_t)x + held_dt[ti][i], 0, 127);
        }
        last = x;
        for (j = 0; j < n && list[j] != x; j++)
            ;
        if (j == n)
            list[n++] = x;
    }
    *cnt = n;
    if (!n)
        return 0;                                   /* (every key silent now: a black key under WHITE since) */
    if (m == AM_REPEAT) {                           /* REPEAT: the last note played, no octaves */
        list[0] = last;
        return 1;
    }
    if (!t->p[P_AORDER] && m != AM_ORD)             /* (ORD: as pressed, held[] keeps that order) */
        for (i = 1; i < n; i++)
            for (j = i; j > 0 && list[j - 1] > list[j]; j--) {
                uint32_t x = list[j];
                list[j] = list[j - 1];
                list[j - 1] = x;
            }
    for (o = 0; o < (uint32_t)t->p[P_AOCT]; o++)
        for (i = 0; i < n && len < 64u; i++)
            list[len++] = (uint32_t)clamp((int32_t)list[i] + 12 * (int32_t)o, 0, 127);
    return len;
}

/* the place in the note list (len notes) the step arp_idx plays, by MODE; UP+8 adds its note to the list */
static uint32_t arp_pick(track_t *t, uint32_t *list, uint32_t len)
{
    uint32_t i = t->arp_idx, m = (uint32_t)t->p[P_AMODE], cyc, k, w;
    if (m == AM_UP8)                                /* UP+8: the first note again over the top (list[] has room) */
        list[len++] = (uint32_t)clamp((int32_t)list[0] + 12 * t->p[P_AOCT], 0, 127);
    cyc = len > 1u ? 2u * len - 2u : 1u;
    k = i % cyc;
    switch (m) {
    case AM_DN:
        return len - 1u - i % len;
    case AM_UPDN:
        return k < len ? k : cyc - k;
    case AM_DNUP:
        return k < len ? len - 1u - k : k - (len - 1u);
    case AM_RND:
        return rng() % len;
    case AM_CONV:
    case AM_DIVG:
        k = i % len;
        if (m == AM_DIVG)
            k = len - 1u - k;
        return (k & 1u) ? len - 1u - k / 2u : k / 2u;
    case AM_PINKY:
        return len < 2u ? 0u : (k & 1u) ? len - 1u : k / 2u;
    case AM_THUMB:
        return len < 2u ? 0u : (k & 1u) ? 1u + k / 2u : 0u;
    case AM_WALK:
        w = t->arp_walk;
        if (!i)
            w = 0;                                  /* (from the bottom) */
        else if (w >= len)
            w = len - 1u;                           /* (notes let go) */
        else if (len > 1u)
            w = !w ? 1u : w == len - 1u ? w - 1u : (rng() >> 7) & 1u ? w + 1u : w - 1u;
        t->arp_walk = (uint8_t)w;
        return w;
    default:                                        /* UP, ORD, REPEAT */
        return i % len;
    }
}

/* the sounding arp note(s) end (CHORD: all of them) */
static void arp_end(track_t *t)
{
    uint32_t i;
    for (i = 0; i < 3u; i++)
        if (t->arp_ch[i]) {
            trk_note_off(t, t->arp_ch[i]);
            seq_out_off(t, t->arp_ch[i]);
            t->arp_ch[i] = 0;
        }
    if (t->arp_note) {
        trk_note_off(t, t->arp_note);
        seq_out_off(t, t->arp_note);
        t->arp_note = 0;
    }
}

/* an arp note starts (recorded while recording: what you hear) */
static void arp_play(track_t *t, uint32_t n)
{
    trk_note_on(t, n, 100);
    seq_out_on(t, n, 100);
    if (((song.rec >> trk_index(t)) & 1u) && song.playing)
        rec_note(t, n, 100, 0, 0);
}

/* an arp step: the next note of the list by MODE, or (CHORD) the first 4 notes together, OCT octaves in turn. 1: it
 * plays (arp_note set), 0: nothing to play */
static int arp_step(track_t *t)
{
    uint32_t list[65], cnt, len = arp_list(t, list, &cnt), i, o, top = 0;
    if (!len)
        return 0;
    t->arp_idx++;
    if (t->p[P_AMODE] != AM_CHORD) {
        t->arp_note = (uint8_t)list[arp_pick(t, list, len)];
        arp_play(t, t->arp_note);
        return 1;
    }
    o = t->arp_idx % (uint32_t)(t->p[P_AOCT] > 0 ? t->p[P_AOCT] : 1);
    for (i = 0; i < cnt && i < 4u; i++) {          /* the held notes (sorted or as played), o octaves up */
        uint32_t nt = (uint32_t)clamp((int32_t)list[i] + 12 * (int32_t)o, 0, 127);
        if (nt == 127u && top++)
            continue;                               /* (notes clamped onto the top one: once) */
        if (!t->arp_note)
            t->arp_note = (uint8_t)nt;
        else
            t->arp_ch[i - 1u] = (uint8_t)nt;
        arp_play(t, nt);
    }
    return 1;
}

/* the arp, once per block: on the transport's grid of RATE (with its SWING) while playing, from the
 * first key while stopped. A new chord starts at once unless the grid is just ahead. While recording,
 * each note it plays is recorded (what you hear) */
static void arp_tick(track_t *t, uint32_t adv)
{
    uint32_t div = (uint32_t)t->p[P_ARATE] % NDIV_SHORT, u = div_units(div), into, slen, abs = 0, fire = 0;
    if (t->arp_note) {
        if (t->arp_off <= adv) {
            arp_end(t);
        } else {
            t->arp_off -= adv;
        }
    }
    if (!ARP_RUNS(t) || !t->nheld) {
        if (!t->nheld && t->arp_note)
            arp_end(t);
        t->arp_new = 0;
        return;
    }
    if (song.playing) {
        abs = grid_at(div, swings(div) ? swing_units(t->p[P_ASWING], u) : 0u, &into, &slen);
        if (t->arp_new) {
            t->arp_new = 0;
            t->arp_abs = into * 4u >= slen * 3u ? abs : abs - 1u;   /* the last quarter: the grid plays it */
        } else if (div != t->arp_den) {
            t->arp_abs = abs;                       /* RATE changed: the next step of the new grid plays (SLOOP 2.4) */
        }
        t->arp_den = (uint8_t)div;
        if (abs + 1u == t->arp_abs)
            abs = t->arp_abs;                       /* ARP SWG turned up inside an odd step: no replay */
        fire = abs != t->arp_abs;
        t->arp_abs = abs;
    } else {                                        /* stopped: from the first key; (2.7) ARP SWG too: the odd
                                                     * steps sw late, as on the grid while playing */
        uint32_t sw = swings(div) ? swing_units(t->p[P_ASWING], u) : 0u, lim;
        if (t->arp_new) {
            t->arp_new = 0;
            t->arp_odd = 0;
            t->arp_pos = u - sw;                    /* (the first note now: the even step's limit) */
        }
        lim = t->arp_odd ? u + sw : u - sw;         /* to an odd step: late; to an even one: back on the grid */
        t->arp_pos += adv;
        if (t->arp_pos >= lim) {
            t->arp_pos = t->arp_pos - lim < lim ? t->arp_pos - lim : 0;
            t->arp_odd ^= 1u;
            fire = 1;
        }
        slen = t->arp_odd ? u + sw : u - sw;        /* (the note's step: to the next) */
    }
    if (!fire)
        return;
    arp_end(t);
    if ((uint32_t)(rng() & 127u) <= (uint32_t)t->p[P_APROB] && arp_step(t))
        t->arp_off = slen * (uint32_t)t->p[P_AGATE] / 128u;
}

/* ------------------------------------------------------- note input --- */
/* armed and stopped, a note on the selected track: an empty project starts a free take, else the
 * note is the downbeat (the transport starts, recording on) */
static void arm_start(track_t *t)
{
    if (!rec_wait || t != TSEL || song.playing || ci_on)
        return;
    if (project_empty() && !rec_tempo) {
        ft_start(t);                              /* the first take sets the loop and the tempo */
    } else if (rec_count) {
        return;                                   /* COUNT: PLAY counts in; a note only sounds */
    } else {
#if FELUCCA_ARRANGER
        arrangement_enabled = 0;
#endif
        seq_start();                              /* the note is the downbeat */
        if (song.playing)
            rec_begin();
    }
}

static void drum_input(uint32_t lane, uint32_t lvl, uint32_t rat, int rec);
/* SLOOP 2.7, GLO > MIDI 3 > DR MAP: the drum sound a MIDI note plays. GM: the General MIDI note of each sound (the nearest
 * one for the others, drums.c lane_of_note), as before; 1-16: 16 notes in a row are the 16 sounds in order, from note 36
 * (KICK, KICK 2, SNARE, CLAP, HAT…) and again every 16 notes, so any keyboard or row of pads plays them all */
/* SLOOP 2.7, GLO > MIDI 3 > REMOTE: on that channel CC 102 + k (64 and above: down, below: up) holds button k, in the
 * order FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+ (panel.c B_*). Kept here by label (the audio ISR);
 * the UI turns them into the panel's buttons (ui_input.c remote_poll). An up waits until the down was seen, so a tap
 * sent as two messages at once is still a tap */
#define REMOTE_CC0 102u
static volatile uint32_t remote_lbl, remote_up;   /* labels held, labels let go (not yet seen down) */
static void remote_cc(uint32_t k, uint32_t value)
{
    uint32_t bit = 1u << k;
    if (value >= 64u) {
        remote_lbl |= bit;
        remote_up &= ~bit;
    } else if (remote_lbl & bit) {
        remote_up |= bit;
    }
}
static uint32_t midi_drum_lane(uint32_t note)
{
    return song.g[G_DRMAP] ? (note + 128u - 36u) % 16u : lane_of_note(note);
}
/* SLOOP 2.6: the sustain pedal (MIDI CC 64, on the track its channel plays): while it is down, a synth track's notes let
 * go keep sounding (their note-off waits: the arp keeps them, a recording holds them) until it is up; a note played
 * again meanwhile is held by its key again. Per track: the pedal, the notes waiting */
static uint8_t sus_on[NTRK];
static uint32_t sus_wait[NTRK][4];
static void input_off_now(track_t *t, uint32_t note);
/* 2.7, KBD HOLD: ARP 2 > HOLD ON with ARP MODE OFF (a synth track). The notes played (keys, MIDI) go on sounding when let
 * go; the next note played with none down replaces them (a new chord), notes played with keys down join them; HOLD OFF,
 * ARP on, another sound or a panic ends them. A key's MIDI note-off waits with its note (the other gear holds too).
 * Recorded as heard: a note ends where it was replaced. (Up to 2.6 HOLD did nothing with the arp OFF) */
static uint32_t kh_set[NTRK][4];                   /* the notes held (bit per MIDI note) */
static uint32_t kh_mo[NTRK][4];                    /* .. of them, a key's: its MIDI note-off waits */
static uint8_t kh_mc[NTRK];                        /* .. on this channel */
static uint8_t kh_phys[NTRK];                      /* note-ons still down (a chord key: one per note) */
static int kh_on(const track_t *t) { return !t->p[P_AMODE] && t->p[P_AHOLD] && !is_drum(t); }
static int kh_has(const track_t *t, uint32_t note)
{
    return (kh_set[trk_index(t) % NTRK][(note >> 5) & 3u] >> (note & 31u)) & 1u;
}
static int kh_any(const track_t *t)
{
    const uint32_t *m = kh_set[trk_index(t) % NTRK];
    return (m[0] | m[1] | m[2] | m[3]) != 0u;
}
static void kh_release(track_t *t)                  /* every held note ends */
{
    uint32_t ti = trk_index(t) % NTRK, w, b;
    for (w = 0; w < 4u; w++)
        for (b = 0; b < 32u; b++)
            if ((kh_set[ti][w] >> b) & 1u) {
                uint32_t n = w * 32u + b;
                if (ft_on && t == &trk[ft_trk % NTRK])
                    ft_note_off(n);
                rec_release(t, n);
                trk_note_off(t, n);
                if ((kh_mo[ti][w] >> b) & 1u)
                    midi_out_note(kh_mc[ti], n, 0);
            }
    memset(kh_set[ti], 0, sizeof kh_set[ti]);
    memset(kh_mo[ti], 0, sizeof kh_mo[ti]);
    kh_phys[ti] = 0;
}
static void input_on(track_t *t, uint32_t note, uint32_t vel)
{
    if (is_drum(t)) {                             /* (a GM note on the drum track: its lane) */
        drum_input(midi_drum_lane(note), vel_lvl(vel), 0, 1);
        return;
    }
    last_note = (uint8_t)note;
    {   /* (2.6) played again under the pedal: the note-off it waited with first (the arp's key count, a recording's
         * note end), then this one held by its key */
        uint32_t *w = &sus_wait[trk_index(t) % NTRK][(note >> 5) & 3u], bit = 1u << (note & 31u);
        if (*w & bit) {
            *w &= ~bit;
            input_off_now(t, note);
        }
    }
    arm_start(t);
    if (kh_on(t)) {                               /* (2.7) KBD HOLD (before the free take's note: the held notes end
                                                   * there first, then this one starts) */
        uint32_t ti = trk_index(t) % NTRK;
        if (!kh_phys[ti])
            kh_release(t);                        /* the first note down: a new chord replaces the held one */
        if (kh_phys[ti] < 255u)
            kh_phys[ti]++;
        if (kh_has(t, note))
            return;                               /* (already held, in this chord: it sounds) */
        kh_set[ti][(note >> 5) & 3u] |= 1u << (note & 31u);
    }
    if (ft_on && t == &trk[ft_trk % NTRK])
        ft_note_on(note, vel);
    if (ARP_RUNS(t)) {
        arp_add(t, note);                         /* (the arp records the notes it plays) */
        return;
    }
    if (((song.rec >> trk_index(t)) & 1u) && song.playing)
        rec_note(t, note, vel, 0, 1);
    trk_note_on(t, note, vel);
}

static void input_off(track_t *t, uint32_t note)
{
    if (is_drum(t)) {
        if (ft_on && ft_trk == TRK_DRUM)
            ft_note_off(midi_drum_lane(note));
        return;
    }
    if (kh_on(t) && kh_has(t, note)) {             /* (2.7) KBD HOLD: it goes on sounding */
        if (kh_phys[trk_index(t) % NTRK])
            kh_phys[trk_index(t) % NTRK]--;
        return;
    }
    if (sus_on[trk_index(t) % NTRK]) {               /* the pedal down: it waits for the pedal (2.6) */
        sus_wait[trk_index(t) % NTRK][(note >> 5) & 3u] |= 1u << (note & 31u);
        return;
    }
    input_off_now(t, note);
}
static void input_off_now(track_t *t, uint32_t note)
{
    if (ft_on && t == &trk[ft_trk % NTRK])
        ft_note_off(note);
    rec_release(t, note);
    arp_remove(t, note);                            /* both: the note may have started in the */
    trk_note_off(t, note);                          /* other mode (ARP switched while held) */
}

/* a drum hit from a key, MIDI or a roll: lane, level; rat: its ratchet when recorded (rolls); rec: it
 * may be recorded (a roll records one hit a step) */
static void drum_input(uint32_t lane, uint32_t lvl, uint32_t rat, int rec)
{
    track_t *t = TDRUM;
    lane &= 15u;
    pen_lane = (uint8_t)lane;
    drum_hand = (uint8_t)(0x80u | lane);            /* (the lane page: the sound played by hand) */
    arm_start(t);
    if (ft_on && ft_trk == TRK_DRUM)
        ft_note_on(lane, lvl);
    if (rec && ((song.rec >> TRK_DRUM) & 1u) && song.playing)
        rec_hit(t, lane, lvl, rat);
    trk_note_on(t, LANE_NOTE[lane], lvl_vel(lvl, 100));
}

/* -------------------------------------------------------------- roll --- */
/* ARP + key: note repeat. The key plays at G_ROLL (1/8, 1/16, 1/32, 32T, 1/64) on the transport's grid
 * (stopped: from the press) until it or ARP is up. Recorded, a roll faster than the track's steps
 * becomes ratchets (1/32 on 1/16 steps: x2, 32T: x3, 1/64: x4). */
static const uint8_t ROLL_DEN[5] = {2, 4, 8, 12, 16};
#define NROLL 4u
static struct {
    uint8_t on, key, trk, note, lvl;    /* note: the synth note, or the lane */
    uint32_t last;                      /* playing: the roll step last played; stopped: units since */
    uint32_t off;                       /* synth: units to its note-off, 0 = not sounding */
    uint32_t rec_abs;                   /* the step its last recorded hit went into */
} roll[NROLL];
static uint32_t roll_den(void) { return ROLL_DEN[(uint32_t)clamp(song.g[G_ROLL], 0, 4)]; }
/* the lanes / notes a roll plays live on track t: the pattern does not play them meanwhile (no flam
 * with what was recorded, nor with what this roll is recording) */
static uint32_t roll_lanes(const track_t *t)
{
    uint32_t r, m = 0;
    for (r = 0; r < NROLL; r++)
        if (roll[r].on && roll[r].trk == trk_index(t))
            m |= 1u << (roll[r].note & 15u);
    return m;
}
static int roll_has(const track_t *t, uint32_t note)
{
    uint32_t r;
    for (r = 0; r < NROLL; r++)
        if (roll[r].on && roll[r].trk == trk_index(t) && roll[r].note == note)
            return 1;
    return 0;
}

static void roll_hit(uint32_t r)
{
    track_t *t = &trk[roll[r].trk % NTRK];
    uint32_t u = BEAT_U / roll_den(), su = div_units((uint32_t)t->p[P_SDIV]);
    uint32_t hits = su / u, rat = hits > 4u ? 3u : hits > 1u ? hits - 1u : 0u;
    uint32_t armed = ((song.rec >> trk_index(t)) & 1u) && song.playing, later, abs = 0, rec = 0;
    if (armed) {                                     /* one recorded hit a step: the ratchet does the rest */
        abs = rec_target(t, &later);
        rec = abs != roll[r].rec_abs;
        roll[r].rec_abs = abs;
    }
    if (is_drum(t)) {
        roll_rec = 1;
        drum_input(roll[r].note, roll[r].lvl, rat, rec || !armed);
        roll_rec = 0;
        seq_out_on(t, LANE_NOTE[roll[r].note & 15u], lvl_vel(roll[r].lvl, 100));
        return;
    }
    arm_start(t);
    if (roll[r].off)
        trk_note_off(t, roll[r].note);
    trk_note_on(t, roll[r].note, lvl_vel(roll[r].lvl, 100));
    seq_out_on(t, roll[r].note, lvl_vel(roll[r].lvl, 100));
    roll[r].off = u / 2u;
    if (rec)
        rec_note(t, roll[r].note, 100, rat, 0);
    if (ft_on && t == &trk[ft_trk % NTRK])
        ft_note_on(roll[r].note, 100), ft_note_off(roll[r].note);
}

static void roll_start(uint32_t k, track_t *t, uint32_t note, uint32_t lvl)
{
    uint32_t r, den = roll_den(), into, slen, abs;
    for (r = 0; r < NROLL && roll[r].on; r++)
        ;
    if (r == NROLL)
        return;
    roll[r].on = 1;
    roll[r].key = (uint8_t)k;
    roll[r].trk = (uint8_t)trk_index(t);
    roll[r].note = (uint8_t)note;
    roll[r].lvl = (uint8_t)lvl;
    roll[r].off = 0;
    roll[r].rec_abs = SEQ_NONE;
    if (song.playing) {
        abs = grid_den(den, 0, &into, &slen);
        roll[r].last = abs;
        if (into * 4u >= slen * 3u)
            return;                                  /* the grid is just ahead: it starts there */
    } else {
        roll[r].last = 0;
    }
    roll_hit(r);
}

static void roll_end(uint32_t r)
{
    if (roll[r].on && roll[r].off && roll[r].trk != TRK_DRUM) {
        trk_note_off(&trk[roll[r].trk % NTRK], roll[r].note);
        seq_out_off(&trk[roll[r].trk % NTRK], roll[r].note);
    }
    if (roll[r].on && roll[r].trk == TRK_DRUM)
        seq_out_off(&trk[TRK_DRUM], LANE_NOTE[roll[r].note & 15u]);
    roll[r].on = 0;
}

static void roll_block(uint32_t adv)
{
    uint32_t r, den = roll_den(), u = BEAT_U / den, into, slen;
    for (r = 0; r < NROLL; r++) {
        if (!roll[r].on)
            continue;
        if (roll[r].off) {
            if (roll[r].off <= adv) {
                trk_note_off(&trk[roll[r].trk % NTRK], roll[r].note);
                seq_out_off(&trk[roll[r].trk % NTRK], roll[r].note);
                roll[r].off = 0;
            } else {
                roll[r].off -= adv;
            }
        }
        if (song.playing) {
            uint32_t abs = grid_den(den, 0, &into, &slen);
            if (abs != roll[r].last) {
                roll[r].last = abs;
                roll_hit(r);
            }
        } else {
            roll[r].last += adv;
            if (roll[r].last >= u) {
                roll[r].last = roll[r].last - u < u ? roll[r].last - u : 0;
                roll_hit(r);
            }
        }
    }
}

/* ------------------------------------------------------------- OMNI --- */
/* ARP MODE OMNI: the keys are a chord harp, after the Omnichord (the chord buttons as FoMni-1 has
 * them). The 11 black keys pick a chord and play it (recorded: a chord step sets the chord again on
 * playback); the 16 white keys are strings over the chord's tones, low to high, played live (not
 * recorded). A button plays the chord it is named for, whatever the scale's ROOT (TRN moves them all).
 * Always polyphonic, also on a MONO track. ARP MODE FLW on another track: its pattern follows the
 * chord's root. */
static const uint8_t OMNI_ROOT[11] = {5, 0, 7, 2, 9, 4, 7, 4, 2, 10, 9};   /* F C G  Dm Am  Em G7 E7  D7 Bb  A7 */
static const uint8_t OMNI_TYPE[11] = {0, 0, 0, 1, 1, 1, 2, 2, 2, 0, 2};    /* major, minor, 7th */
static const int8_t OMNI_PAD[3][3] = {{0, 4, 7}, {0, 3, 7}, {0, 4, 10}};    /* the chord played */
static const int8_t OMNI_STR[3][4] = {{0, 4, 7, -1}, {0, 3, 7, -1}, {0, 4, 7, 10}};   /* the strings */
static const char *const OMNI_NOTE[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static const char *const OMNI_SUF[3] = {"", "m", "7"};
#define OM_BLACK(k) ((0x54Au >> ((53u + (k)) % 12u)) & 1u)   /* key k is F3 + k */
static volatile uint8_t omni_ch = 0xFFu;        /* the chord now: 0..10, 0xFF none yet (C) */
static volatile uint8_t omni_pc;                /* its root, a pitch class (the name) */
static volatile uint8_t omni_new;               /* the UI says the chord (ui_draw) */

static uint32_t omni_keypc(const track_t *t)    /* the chords' transposition: TRN */
{
    return (uint32_t)((t->p[P_TRANS] % 12 + 12) % 12);
}

/* a note of the harp: polyphonic whatever the track's VOICE (a MONO track would play one tone of
 * the chord); the note-off finds it as any POLY voice */
static void omni_on(track_t *t, uint32_t note, int input)
{
    int16_t vm = t->p[P_VOICE];
    t->p[P_VOICE] = V_POLY;
    if (input)
        input_on(t, note, 100);
    else
        trk_note_on(t, note, 100);
    t->p[P_VOICE] = vm;
}

static void omni_set(const track_t *t, uint32_t c, int say)   /* say: the message bar names it (a key, not a step) */
{
    omni_ch = (uint8_t)c;
    omni_pc = (uint8_t)((omni_keypc(t) + OMNI_ROOT[c]) % 12u);
    if (say)
        omni_new = 1;
}

static void omni_name(char *b)                  /* "Am", "G7" */
{
    uint32_t c = omni_ch < 11u ? omni_ch : 1u;
    str_cpy(b, OMNI_NOTE[omni_pc % 12u], 4);
    str_cpy(b + str_len(b), OMNI_SUF[OMNI_TYPE[c]], 4);
}

/* the semitones FLW moves a pattern (written in C) by: to the chord's root, the nearer way (-5..+6) */
static int32_t omni_shift(void)
{
    int32_t r = omni_ch < 11u ? omni_pc % 12 : 0;
    return r > 6 ? r - 12 : r;
}

/* the notes of chord c: its root from F3 to E4 (the octave buttons move it) */
static uint32_t omni_pad(const track_t *t, uint32_t c, uint8_t *nt)
{
    int32_t r = 53 + (int32_t)((omni_keypc(t) + OMNI_ROOT[c] + 7u) % 12u) + 12 * song.octave, i;
    for (i = 0; i < 3; i++)
        nt[i] = (uint8_t)clamp(r + OMNI_PAD[OMNI_TYPE[c]][i], 0, 127);
    return 3;
}

/* string s (0 = the lowest): the s-th tone of the chord from G3 up */
static uint32_t omni_string(const track_t *t, uint32_t s)
{
    uint32_t c = omni_ch < 11u ? omni_ch : 1u, root = (omni_keypc(t) + OMNI_ROOT[c]) % 12u, j;
    const int8_t *tone = OMNI_STR[OMNI_TYPE[c]];
    int32_t n = 55 + 12 * song.octave;
    for (; n < 120; n++) {
        uint32_t iv = (uint32_t)(n - (int32_t)root + 120) % 12u;
        for (j = 0; j < 4u && tone[j] >= 0 && (uint32_t)tone[j] != iv; j++)
            ;
        if (j < 4u && tone[j] >= 0 && !s--)
            break;
    }
    return (uint32_t)clamp(n, 0, 127);
}

/* a chord step of an OMNI track (played back): the chord it holds becomes the chord now */
static void omni_detect(const track_t *t, const step_t *s)
{
    uint32_t c, i, m = 0;
    for (i = 0; i < s->n; i++)
        m |= 1u << (s->note[i] % 12u);
    for (c = 0; c < 11u; c++) {
        uint8_t nt[3];
        uint32_t w = 0;
        omni_pad(t, c, nt);
        for (i = 0; i < 3u; i++)
            w |= 1u << (nt[i] % 12u);
        if (w == m) {
            if (c != omni_ch)
                omni_set(t, c, 0);                  /* (played back: no message over the screen) */
            return;
        }
    }
}

/* the index of key k among the black (1) or the white (0) keys */
static uint32_t omni_idx(uint32_t k, uint32_t black)
{
    uint32_t i, n = 0;
    for (i = 0; i < k; i++)
        n += OM_BLACK(i) == black;
    return n;
}

static void omni_down(track_t *t, uint32_t k, uint32_t sel)
{
    uint32_t i, mc = trk_midi_ch(sel);
    if (OM_BLACK(k)) {                                  /* a chord button: the chord, played and recorded */
        uint32_t c = omni_idx(k, 1);
        if (c >= 11u)
            return;
        omni_set(t, c, 1);
        kb_kind[k] = KS_NOTE;
        kb_n[k] = (uint8_t)omni_pad(t, c, kb_nt[k]);
        for (i = 0; i < kb_n[k]; i++) {
            omni_on(t, kb_nt[k][i], 1);
            midi_out_event(0x09u | (0x90u | mc) << 8 | (uint32_t)kb_nt[k][i] << 16 | 100u << 24);
        }
        return;
    }
    kb_kind[k] = KS_STRUM;                              /* a string: a tone of the chord, recorded as its note (2.9) */
    kb_nt[k][0] = (uint8_t)omni_string(t, omni_idx(k, 0));
    kb_n[k] = 1;
    omni_on(t, kb_nt[k][0], 1);
    midi_out_event(0x09u | (0x90u | mc) << 8 | (uint32_t)kb_nt[k][0] << 16 | 100u << 24);
}

/* a step's note: on an OMNI track the chord sounds whole whatever VOICE (as the keys play it, omni_on) */
static void seq_on(track_t *t, uint32_t note, uint32_t vel)
{
    int16_t vm = t->p[P_VOICE];
    if (t->p[P_AMODE] == AM_OMNI)
        t->p[P_VOICE] = V_POLY;
    trk_note_on(t, note, vel);
    t->p[P_VOICE] = vm;
}

/* ---------------------------------------------------------- keyboard --- */
/* the level of a key on the drum track: OCT- held ghost, OCT+ held hard */
static uint32_t key_lvl(void)
{
    uint32_t b = fm1_in.buttons;
    return (b & dyn_bit[0]) ? LV_GHOST : (b & dyn_bit[1]) ? LV_HARD : LV_NORM;
}

/* 2.7: the punch-in FX over MIDI. On GLO > MIDI 3's FX CH, the notes F3 .. G5 (53 .. 79) are the FM-1's 27 keys with
 * FX held: a note on holds its effect (white keys 1..16, black keys 17..27), its note off lets it go. A key of the
 * FM-1's own (FX + key) takes over at once, and its release ends it; a note off then leaves it alone */
static uint8_t mfx_key;                          /* the key the FX channel holds, + 1 (0: none) */
static void midi_fx(uint32_t note, int on)
{
    int32_t fx;
    uint32_t k;
    if (note < 53u || note > 79u)
        return;
    k = note - 53u;
    fx = punch_fx_of_key(k);
    if (fx < 0 || fx >= (int32_t)PUNCH_NFX)
        return;
    if (on) {
        punch.req = (int8_t)fx;
        punch.keybit = 0;                        /* (no FM-1 key holds it: a key's release does not end it) */
        mfx_key = (uint8_t)(k + 1u);
    } else if (mfx_key == k + 1u) {
        mfx_key = 0;
        if (!punch.keybit)                       /* (an FM-1 key took it over: its release ends it) */
            punch.req = -1;
    }
}

static void key_down(uint32_t k)
{
    uint32_t layer = layer_now(), sel = song.sel % NTRK, i, mc;
    track_t *t = &trk[sel];
    kb_kind[k] = KS_NONE;
    kb_trk[k] = (uint8_t)sel;
    kb_n[k] = 0;
    switch (layer) {
    case LY_FX: {                                     /* FX held: the keys pick a punch-in effect (white and black) */
        int32_t fx = punch_fx_of_key(k);
        kb_kind[k] = KS_FX;
        if (fx >= 0 && fx < (int32_t)PUNCH_NFX) {
            punch.req = (int8_t)fx;
            punch.keybit = 1u << k;
        }
        return;
    }
    case LY_STEP:
    case LY_SCALE:
    case LY_MIX:
    case LY_SONG:                                     /* the UI's: steps, the key, the mix, the sections */
        kb_kind[k] = KS_UI;
        kb_nt[k][0] = (uint8_t)layer;                 /* (its key-up goes to the same layer) */
        lk_push(layer, k, 1);
        return;
    case LY_ERASE:
        if (is_drum(t) && punch_key(k) < 0) {        /* (2.6) a black key: erase <-> mute */
            er_mute = (uint8_t)!er_mute;
            return;
        }
        if (is_drum(t) && er_mute) {                  /* MUTE: the white key's sound off / on in the sequencer */
            drum_mute ^= (uint16_t)(1u << lane_of_key(k));
            return;
        }
        kb_kind[k] = KS_ERASE;
        if (!erasing(t) || er_trk != sel)
            undo_erase_sess = (undo_sess += 4u) | 2u;  /* a new erase: one undo */
        if (er_trk != sel) {
            er_lanes = 0;
            er_notes[0] = er_notes[1] = er_notes[2] = er_notes[3] = 0;
            er_trk = (uint8_t)sel;
        }
        if (is_drum(t)) {
            kb_nt[k][0] = (uint8_t)lane_of_key(k);
            kb_n[k] = 1;
            er_lanes |= (uint16_t)(1u << kb_nt[k][0]);
        } else {
            uint32_t n = kb_map(t, k);
            if (n == KB_SILENT)
                return;
            kb_n[k] = (uint8_t)(t->p[P_CHORD] ? chord_notes(t, n, kb_nt[k]) : 1u);
            if (!t->p[P_CHORD])
                kb_nt[k][0] = (uint8_t)n;
            for (i = 0; i < kb_n[k]; i++)
                er_notes[(kb_nt[k][i] >> 5) & 3u] |= 1u << (kb_nt[k][i] & 31u);
        }
        erase_now(t);
        return;
    default:
        break;
    }
    if (is_drum(t)) {                                 /* the drum track: the key's lane */
        uint32_t lane = lane_of_key(k), lvl = key_lvl();
        kb_nt[k][0] = (uint8_t)lane;
        kb_n[k] = 1;
        if (layer == LY_ROLL) {
            kb_kind[k] = KS_ROLL;
            roll_start(k, t, lane, lvl);
            return;
        }
        kb_kind[k] = KS_DRUM;
        drum_input(lane, lvl, 0, 1);
        mc = trk_midi_ch(sel);
        midi_out_note(mc, (uint32_t)LANE_NOTE[lane], lvl_vel(lvl, 100));
        return;
    }
    if (t->p[P_AMODE] == AM_OMNI && layer != LY_ROLL) {
        omni_down(t, k, sel);
        return;
    }
    {
        uint32_t n = kb_map(t, k);
        if (n == KB_SILENT)
            return;
        if (layer == LY_ROLL) {
            kb_kind[k] = KS_ROLL;
            kb_nt[k][0] = (uint8_t)n;
            kb_n[k] = 1;
            roll_start(k, t, n, LV_NORM);
            return;
        }
        kb_kind[k] = KS_NOTE;
        kb_root[k] = (uint8_t)n;
        kb_oct[k] = (int8_t)song.octave;
        if (t->p[P_CHORD]) {
            kb_n[k] = (uint8_t)chord_notes(t, n, kb_nt[k]);
        } else {
            kb_nt[k][0] = (uint8_t)n;
            kb_n[k] = 1;
        }
        mc = trk_midi_ch(sel);
        in_key = (uint8_t)(k + 1u);                 /* (the arp keeps the key: arp_add, #191) */
        for (i = 0; i < kb_n[k]; i++) {
            input_on(t, kb_nt[k][i], 100);
            midi_out_note(mc, (uint32_t)kb_nt[k][i], 100u);
        }
        in_key = 0;
        /* the pen of the SEQ layer: the keys down now (a chord), else this note */
        if (!(kb_prev & ~(1u << k)) || pen_n >= 4u)
            pen_n = 0;
        for (i = 0; i < kb_n[k] && pen_n < 4u; i++)
            pen_note[pen_n++] = kb_nt[k][i];
    }
}

static void key_up(uint32_t k)
{
    uint32_t i, mc, kind = kb_kind[k];
    track_t *t = &trk[kb_trk[k] % NTRK];
    kb_kind[k] = KS_NONE;
    switch (kind) {
    case KS_FX:
        if (punch.keybit == 1u << k) {                /* its key is up: the mix comes back */
            punch.keybit = 0;
            punch.req = -1;
        }
        return;
    case KS_UI:
        lk_push(kb_nt[k][0], k, 0);
        return;
    case KS_ERASE:
        if (is_drum(t))
            er_lanes &= (uint16_t)~(1u << kb_nt[k][0]);
        else
            for (i = 0; i < kb_n[k]; i++)
                er_notes[(kb_nt[k][i] >> 5) & 3u] &= ~(1u << (kb_nt[k][i] & 31u));
        return;
    case KS_ROLL:
        for (i = 0; i < NROLL; i++)
            if (roll[i].on && roll[i].key == k)
                roll_end(i);
        return;
    case KS_NOTE:
        mc = trk_midi_ch(kb_trk[k] % NTRK);
        for (i = 0; i < kb_n[k]; i++) {
            uint32_t n = kb_nt[k][i];
            input_off(t, n);
            if (kh_on(t) && kh_has(t, n)) {         /* (2.7) KBD HOLD: its MIDI note-off waits with it */
                kh_mo[kb_trk[k] % NTRK][(n >> 5) & 3u] |= 1u << (n & 31u);
                kh_mc[kb_trk[k] % NTRK] = (uint8_t)mc;
            } else {
                midi_out_note(mc, n, 0);
            }
        }
        return;
    case KS_STRUM:
        input_off(t, kb_nt[k][0]);
        mc = trk_midi_ch(kb_trk[k] % NTRK);
        midi_out_event(0x08u | (0x80u | mc) << 8 | (uint32_t)kb_nt[k][0] << 16);
        return;
    case KS_DRUM:
        if (ft_on && ft_trk == TRK_DRUM)
            ft_note_off(kb_nt[k][0]);
        mc = trk_midi_ch(TRK_DRUM);
        midi_out_note(mc, (uint32_t)LANE_NOTE[kb_nt[k][0] & 15u], 0);
        return;
    default:
        return;
    }
}

static void keyboard_block(void)
{
    uint32_t cur = fm1_in.notes, ch = cur ^ kb_prev, k, r;
    /* (2.7: MUTE mode stays when EDIT is let go, until a black key again: the mutes at hand, live; 2.6 went back to
     * erasing) */
    if (!(layer_buttons() & ly_bit[LY_ROLL]))         /* ARP up (and not locked): the rolls end (the keys stay silent) */
        for (r = 0; r < NROLL; r++)
            if (roll[r].on && roll[r].key < 27u)        /* (2.7: not the LIVE page's, key 0x80 + note: its pad ends them) */
                roll_end(r);
    if (!ch)
        return;
    for (k = 0; k < 27u; k++) {
        if (!((ch >> k) & 1u))
            continue;
        if ((cur >> k) & 1u)
            key_down(k);
        else
            key_up(k);
        kb_prev ^= 1u << k;                           /* (key_down sees the keys down before it) */
    }
}

/* LIVE: one record arm at most, on the selected track; selecting another track moves it there
 * (an arm follows too: it waits for a note on the track selected; a free take stays on its track) */
static void rec_follow(uint32_t sel)
{
    if (song.rec)
        song.rec = (uint8_t)(1u << (sel % NTRK));
}

/* -------------------------------------------------------- metronome --- */
/* A wood block on every beat, louder on the first of the bar, from the drum kit's voices
 * (not recorded, not muted with the drum track; GLO > DRUMS LVL sets its level).
 * GLO > GLOBAL CLICK: OFF (0, default: LIVE needs none), REC = while a track records,
 * ON = while playing. */
#define CLICK_REC 1
#define CLICK_ON 2
static uint32_t click_last = SEQ_NONE;          /* the beat it last played */
static void click_tick(void)
{
    uint32_t mode = (uint32_t)song.g[G_CLOCK];
    if (!song.playing || clk_beat == click_last)
        return;
    click_last = clk_beat;
    if (mode == CLICK_ON || (mode == CLICK_REC && song.rec))
        drum_on(clk_beat % 4u ? 76u : 77u, clk_beat % 4u ? 72u : 120u);
}

/* -------------------------------------------------------- sequencer --- */
#if FELUCCA_ARRANGER
/* ---- LIVE SECTIONS (SAVE held + key, ui_layers.c): a section asked for while playing starts on the
 * next bar, every track from its step 0 (as the song does). SONG REC writes the order you play into the
 * song chain (arrangement): each section with the bars it played, from the bar after the arm. */
static volatile int8_t live_req = -1;              /* section asked for (UI), applied on the next bar */
static volatile int8_t live_sec = -1;              /* the section playing: last jumped to, loaded or stored */
static uint32_t live_bar = 0xFFFFFFFFu;            /* clk_beat / 4 of the last bar seen (= bars the section played) */
/* QUICK CHAIN (SLOOP 2.4; SAVE held, two or more section keys tapped): the sections in order, looped, each for the
 * bars its longest pattern takes (section_bars); the UI writes it whole with the IRQ off, chain_n last (0 = none) */
#define CHAIN_MAX 8u
static volatile uint8_t chain_sec[CHAIN_MAX];
static volatile uint8_t chain_n, chain_i;          /* entries; the one playing (or asked for) */
static volatile uint8_t chain_bars;                /* the bars it plays */
static uint32_t section_bars(uint32_t s);         /* arranger_scene.c */
static volatile uint8_t srec;                      /* SONG REC: 0 off, 1 armed (from the next bar), 2 recording */
static arr_entry_t srec_e[ARR_STEPS];
static volatile uint8_t srec_n;                    /* entries so far (the last one still growing) */
static volatile uint8_t srec_done;                 /* the chain was written: n parts (0xFF: nothing played) */

static void srec_finish(void)                      /* (audio ISR, or the UI with the IRQ off) */
{
    uint32_t i, n = 0;
    for (i = 0; i < srec_n; i++)
        if (srec_e[i].bars)
            srec_e[n++] = srec_e[i];
    if (n) {
        arrangement.count = (uint8_t)n;
        arrangement.loop = 0;
        for (i = 0; i < n; i++)
            arrangement.entry[i] = srec_e[i];
    }
    srec_done = (uint8_t)(n ? n : 0xFFu);
    srec = 0;
    srec_n = 0;
}
static void srec_add(uint32_t s)
{
    if (srec_n >= ARR_STEPS) {
        srec_finish();                              /* the chain is full: what was played so far */
        return;
    }
    srec_e[srec_n].scene = (uint8_t)s;
    srec_e[srec_n].bars = 0;
    srec_n++;
}
/* STOP (or SONG REC pressed again): the bar playing counts if it had begun */
static void srec_stop(void)
{
    if (srec == 2u && srec_n && srec_e[srec_n - 1u].bars < 64u &&
        ((!(clk_beat & 3u) && (clk_beat >> 2) != live_bar) || (clk_beat & 3u)))
        srec_e[srec_n - 1u].bars++;
    if (srec == 2u)
        srec_finish();
    srec = 0;
}
static void seq_reset_tracks(uint32_t pos);
static void live_block(void)                       /* once a block while playing a loop (not the song) */
{
    if ((clk_beat & 3u) || (clk_beat >> 2) == live_bar)
        return;
    live_bar = clk_beat >> 2;                       /* a new bar */
    if (srec == 2u && srec_n) {
        arr_entry_t *e = &srec_e[srec_n - 1u];
        if (e->bars < 64u) {
            e->bars++;
        } else {                                    /* (64 bars of one section: it goes on in the next entry) */
            srec_add(e->scene);
            if (srec == 2u)
                srec_e[srec_n - 1u].bars = 1;
        }
    }
    if (chain_n && live_req < 0 && live_bar >= chain_bars) {   /* the chain: this entry has played its bars */
        chain_i = (uint8_t)((chain_i + 1u) % (chain_n < CHAIN_MAX ? chain_n : CHAIN_MAX));
        live_req = (int8_t)(chain_sec[chain_i] & 3u);
    }
    if (live_req >= 0) {
        uint32_t s = (uint32_t)live_req;
        live_req = -1;
        {                                           /* (the UI asked for a section it checked: no hash here) */
            arrangement_apply(s);
            song.rec = 0;                           /* (a take does not run on into another section) */
            live_sec = (int8_t)s;
            seq_reset_tracks(clk_pos);              /* on the bar: every track from its step 0 */
            live_bar = 0;
            if (chain_n)
                chain_bars = (uint8_t)section_bars(chain_sec[chain_i % CHAIN_MAX]);
            if (srec == 2u)
                srec_add(s);
        }
    }
    if (srec == 1u && live_sec >= 0) {
        srec = 2;
        srec_n = 0;
        srec_add((uint32_t)live_sec);
    }
}
#endif

/* every track from its step 0, together, at pos units into beat 0 (a song section: the arranger's
 * remainder, so the new section starts exactly on its bar) */
static void seq_reset_tracks(uint32_t pos)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        t->seq_abs = SEQ_NONE;
        t->seq_idx = 0;
        t->rskip_n = 0;
        t->rskip_lanes = 0;
        t->rskip_abs = SEQ_NONE;
        t->rh_n = 0;
        t->arp_new = t->nheld != 0;
    }
    for (i = 0; i < NROLL; i++)
        roll[i].last = SEQ_NONE - 1u;               /* (a roll held over the start: on the grid from here) */
    clk_beat = 0;
    clk_pos = pos;
#if FELUCCA_ARRANGER
    live_bar = 0xFFFFFFFFu;                        /* (bar 0 is a new bar: SONG REC can start on it) */
#endif
    click_last = SEQ_NONE;
    song.tick = 0;
    song.playing = 1;
    slicer_start(pos);                             /* slicer.c: its step 0 with the sequencer's */
}


static void song_backup(void);                     /* project.c: song mode keeps the loop you made */
static void song_restore(void);

static void seq_start(void)
{
#if FELUCCA_ARRANGER
    if (arrangement_enabled && !song.playing) {
        rec_wait = 0;                              /* song mode plays, it does not record */
        song_backup();
        chain_n = 0;                               /* (the song, not a quick chain) */
    }
    if (!arrangement_start()) return;
#endif
    seq_reset_tracks(0);
}

static void seq_release(track_t *t)
{
    uint32_t i;
    for (i = 0; i < t->seq_n; i++) {
        trk_note_off(t, t->seq_notes[i]);
        seq_out_off(t, t->seq_notes[i]);
    }
    if (is_drum(t))
        seq_out_track_off(t);                       /* the drum hits of the step: ended with it */
    t->seq_n = 0;
    t->seq_hold = 0;
    t->slide_glide = 0;                             /* live MONO / LEG keys must not glide after it */
}

static void seq_stop(void)
{
    uint32_t i;
#if FELUCCA_ARRANGER
    if (song.playing)
        srec_stop();                                /* SONG REC: the order played so far is the song */
    live_req = -1;
    chain_n = 0;
#endif
    song.playing = 0;
    for (i = 0; i < NTRK; i++) {
        seq_release(&trk[i]);
        trk[i].rh_n = 0;                           /* a recorded note held over the stop: as far as it got */
    }
    seq_out_all_off();
#if FELUCCA_ARRANGER
    if (arrangement_clock.running) {
        arrangement_clock.running = 0;
        song_restore();                            /* back to the loop you were making */
    }
#endif
}

/* a step's note as it sounds: FLW moves it by the shift the step started with */
static uint32_t flw_note(const track_t *t, uint32_t n)
{
    return (uint32_t)clamp((int32_t)n + t->flw_sh, 0, 127);
}

/* the velocity of note i of synth step s */
static uint32_t step_vel(const step_t *s, uint32_t i)
{
    uint32_t base = (s->flags & SF_ACCENT) ? 127u : (s->vel ? s->vel : 96u);
    return lvl_vel((s->lvl >> (2u * i)) & 3u, base);
}

/* play one synth step: TIE extends, REST releases, NOTE (re)triggers; a SLIDE on the previous
 * step makes this one legato with a glide (acid style). skip: bit k = note k already sounds
 * from live recording (not triggered, not released here). len: the step's length (units). */
static void seq_step(track_t *t, const step_t *s, uint32_t slen, uint32_t skip)
{
    uint32_t i, j, gate = slen * (uint32_t)t->p[P_SGATE] / 128u;
    uint32_t slide_in = t->seq_hold && t->seq_n;
    const step_t *nx = &t->step[(t->seq_idx + 1u) % trk_len(t)];
    uint32_t next_tie = nx->time == ST_TIE || (nx->time == ST_NOTE && (nx->flags & SF_CONT_ALL));   /* (2.7: notes going on
                                                     * into the next step: held to it, it says which end) */
    if (s->time == ST_TIE) {
        if (t->seq_n) {
            t->seq_off = gate + slen / 2u;
            t->seq_hold = (s->flags & SF_SLIDE) != 0 || next_tie;   /* chains hold at any GATE / swing */
        }
        return;
    }
    if (s->time == ST_REST || !s->n) {
        seq_release(t);
        return;
    }
    if (s->flags & SF_CONT_ALL) {                   /* (2.7) notes going on: kept; the others end, the new ones start */
        uint8_t old[4];
        uint32_t no = t->seq_n > 4u ? 4u : t->seq_n, kept = 0;
        memcpy(old, t->seq_notes, no);
        for (j = 0; j < no; j++) {
            for (i = 0; i < s->n && !((s->flags & SF_CONT(i)) && s->note[i] == old[j] && !((kept >> i) & 1u)); i++)
                ;
            if (i < s->n) {
                kept |= 1u << i;
            } else {
                trk_note_off(t, old[j]);
                seq_out_off(t, old[j]);
            }
        }
        t->slide_glide = 0;
        for (i = 0; i < s->n; i++)
            if (!((kept >> i) & 1u) && !((skip >> i) & 1u) && !roll_has(t, s->note[i])) {
                trk_note_on(t, s->note[i], step_vel(s, i));   /* (a note going on that was not playing: from here) */
                seq_out_on(t, s->note[i], step_vel(s, i));
            }
        t->seq_n = 0;
        for (i = 0; i < s->n; i++)
            if (!((skip >> i) & 1u))
                t->seq_notes[t->seq_n++] = s->note[i];
        t->seq_off = gate;
        t->seq_hold = (s->flags & SF_SLIDE) != 0 || next_tie;
        return;
    }
    if (s->rat) {                                   /* ratchets: each hit its share of the step */
        uint32_t hits = 1u + ((s->rat >> 0) & 3u);
        for (i = 1; i < s->n; i++)
            if (1u + ((s->rat >> (2u * i)) & 3u) > hits)
                hits = 1u + ((s->rat >> (2u * i)) & 3u);
        gate /= hits;
    }
    t->slide_glide = (uint8_t)slide_in;
    if (!slide_in)
        seq_release(t);
    if (t->p[P_AMODE] == AM_OMNI && s->n >= 3u)
        omni_detect(t, s);                          /* a chord step: the chord the strings and FLW play */
    t->flw_sh = (int8_t)(t->p[P_AMODE] == AM_FLW ? omni_shift() : 0);
    for (i = 0; i < s->n; i++)
        if (roll_has(t, s->note[i]))
            skip |= 1u << i;                        /* (a roll plays it) */
    for (i = 0; i < s->n; i++)
        if (!((skip >> i) & 1u)) {
            uint32_t n = flw_note(t, s->note[i]);
            seq_on(t, n, step_vel(s, i));
            if (!slide_in || !seq_out_is_on(t, n))
                seq_out_on(t, n, step_vel(s, i));   /* (a slide into the same note: one MIDI note) */
        }
    if (slide_in)                                   /* release what is not held over */
        for (i = 0; i < t->seq_n; i++) {
            for (j = 0; j < s->n && flw_note(t, s->note[j]) != t->seq_notes[i]; j++)
                ;
            if (j == s->n) {
                trk_note_off(t, t->seq_notes[i]);
                seq_out_off(t, t->seq_notes[i]);
            }
        }
    t->seq_n = 0;
    for (i = 0; i < s->n; i++)
        if (!((skip >> i) & 1u))
            t->seq_notes[t->seq_n++] = (uint8_t)flw_note(t, s->note[i]);
    t->seq_off = gate;
    t->seq_hold = !s->rat && ((s->flags & SF_SLIDE) != 0 || next_tie);   /* next step a TIE: keep the notes to it */
}

/* play one drum step: each lane a hit (skip: lanes already played by live recording, or rolling) */
static void drum_step(track_t *t, const dstep_t *s, uint32_t skip)
{
    uint32_t l, m = dstep_mask(s) & ~skip & ~roll_lanes(t) & ~(uint32_t)drum_mute;   /* (4.0: the live mutes) */
    drum_lock = dext.lock[(uint32_t)(s - t->dstep) % NSTEP];   /* the step's TUNE / DECAY lock of one lane */
    seq_out_track_off(t);                           /* the last step's hits end here */
    for (l = 0; m; l++, m >>= 1)
        if (m & 1u) {
            trk_note_on(t, LANE_NOTE[l], lvl_vel(dstep_lvl(s, l), 100));
            seq_out_on(t, LANE_NOTE[l], lvl_vel(dstep_lvl(s, l), 100));
        }
    drum_lock = 0;
}

/* ratchets: the further hits of the playing step's notes / lanes, each at its share of the step */
static void seq_ratchets(track_t *t, uint32_t into, uint32_t slen)
{
    uint32_t i;
    if (is_drum(t)) {
        const dstep_t *s = &t->dstep[t->seq_idx % NSTEP];
        uint32_t m = dstep_mask(s) & ~roll_lanes(t) & ~(uint32_t)drum_mute;
        for (i = 0; m; i++, m >>= 1) {
            uint32_t hits = 1u + dstep_rat(s, i), h, done;
            if (!(m & 1u) || hits == 1u)
                continue;
            h = into * hits / slen;
            done = (t->rat_lanes >> (2u * i)) & 3u;
            if (h > done && h < hits) {
                t->rat_lanes = (t->rat_lanes & ~(3u << (2u * i))) | h << (2u * i);
                drum_lock = dext.lock[t->seq_idx % NSTEP];
                trk_note_on(t, LANE_NOTE[i], lvl_vel(dstep_lvl(s, i), 100));
                seq_out_on(t, LANE_NOTE[i], lvl_vel(dstep_lvl(s, i), 100));
                drum_lock = 0;
            }
        }
        return;
    }
    {
        const step_t *s = &t->step[t->seq_idx % NSTEP];
        if (s->time != ST_NOTE || !s->rat)
            return;
        for (i = 0; i < s->n; i++) {
            uint32_t hits = 1u + ((s->rat >> (2u * i)) & 3u), h;
            if (hits == 1u || roll_has(t, s->note[i]))
                continue;
            h = into * hits / slen;
            if (h > t->rat_done[i] && h < hits) {
                uint32_t j;
                uint32_t n = flw_note(t, s->note[i]);
                t->rat_done[i] = (uint8_t)h;
                trk_note_off(t, n);
                seq_on(t, n, step_vel(s, i));
                seq_out_on(t, n, step_vel(s, i));
                t->seq_off = slen / hits * (uint32_t)t->p[P_SGATE] / 128u;
                for (j = 0; j < t->seq_n && t->seq_notes[j] != n; j++)
                    ;
                if (j == t->seq_n && t->seq_n < 4u)
                    t->seq_notes[t->seq_n++] = (uint8_t)n;   /* (its gate ends it) */
            }
        }
    }
}

static void seq_tick(track_t *t, uint32_t adv)
{
    uint32_t len = trk_len(t), into, slen, abs, idx;
    if (t->seq_n && !t->seq_hold) {
        if (t->seq_off <= adv)
            seq_release(t);
        else
            t->seq_off -= adv;
    }
    if (!song.playing)
        return;
    abs = trk_grid(t, &into, &slen);
    {
        uint32_t div = trk_div(t);
        if (t->seq_abs != SEQ_NONE && div != t->seq_den)
            t->seq_abs = abs;                        /* DIV changed: the next step of the new grid plays (SLOOP 2.4) */
        t->seq_den = (uint8_t)div;
    }
    if (t->seq_abs != SEQ_NONE && abs + 1u == t->seq_abs)
        abs = t->seq_abs;                            /* SWING turned up inside a played odd step */
    if (abs != t->seq_abs) {                         /* a new step: one a block at most */
        t->seq_abs = abs;
        idx = abs % len;
        t->seq_idx = (uint16_t)idx;
        t->rat_done[0] = t->rat_done[1] = t->rat_done[2] = t->rat_done[3] = 0;
        t->rat_lanes = 0;
        if (!idx)
            t->pass++;                               /* a new pass of the loop (recording: one undo) */
        if (erasing(t))
            erase_step(t, idx);                      /* EDIT + key held: gone as it passes */
        if (is_drum(t)) {
            uint32_t skip = t->rskip_abs == abs ? t->rskip_lanes : 0u;
            t->rskip_lanes = 0;
            drum_step(t, &t->dstep[idx], skip);
        } else {
            const step_t *s = &t->step[idx];
            uint32_t skip = 0, i, k;
            rec_hold(t, idx, len, abs);
            if (t->rskip_n && t->rskip_abs == abs)
                for (i = 0; i < s->n; i++)
                    for (k = 0; k < t->rskip_n; k++)
                        if (s->note[i] == t->rskip[k])
                            skip |= 1u << i;
            t->rskip_n = 0;
            seq_step(t, s, slen, skip);
        }
    }
    seq_ratchets(t, into, slen);
}

static volatile uint8_t midi_pc[NTRK];               /* a MIDI Program Change per track: program + 1 (ui.c midi_pc_take) */

/* MIDI in: the track that has channel ch (0..15) as its own: the drum track's GLO > DRUMS > CH, synth track i's
 * GLO > MIDI > CHi (4.0, SLOOP 2.6; 1, 2, 3 as before; a channel two tracks have: the first of them), else 0 */
static track_t *midi_own(uint32_t ch)
{
    uint32_t i;
    if (song.g[G_DRCH] && ch + 1u == (uint32_t)song.g[G_DRCH])
        return TDRUM;
    for (i = 0; i < NPART; i++)
        if (ch + 1u == (uint32_t)song.g[G_CH1 + i])
            return &trk[i];
    return 0;
}
/* the track a channel plays: its own, or (GLO > MIDI > OTHER = SEL) the selected one; 0: none (OTHER = OFF) */
static track_t *midi_track(uint32_t ch)
{
    track_t *t = midi_own(ch);
    return t ? t : song.g[G_CHOTH] ? 0 : TSEL;
}

/* a channel that plays the selected track: its note-off goes to the track its note-on went to,
 * even when another track was selected in between (else that note would hang) */
static uint8_t midi_sel_on[16][64];                   /* per channel and note, a nibble: track + 1, 0 = none (sloop-lab:
                                                       * 1 KB, not 2: the value is 0..NTRK) */
static inline uint32_t midi_sel_get(uint32_t ch, uint32_t note)
{
    return (midi_sel_on[ch & 15u][(note & 127u) >> 1] >> (4u * (note & 1u))) & 15u;
}
static inline void midi_sel_set(uint32_t ch, uint32_t note, uint32_t v)
{
    uint8_t *b = &midi_sel_on[ch & 15u][(note & 127u) >> 1];
    uint32_t sh = 4u * (note & 1u);
    *b = (uint8_t)((*b & ~(15u << sh)) | ((v & 15u) << sh));
}
static track_t *midi_route(uint32_t ch, uint32_t note, int on)
{
    track_t *t = midi_own(ch);
    if (t)
        return t;                                     /* a track's own channel (GLO > MIDI, GLO > DRUMS > CH) */
    if (song.g[G_CHOTH] && (on || !midi_sel_get(ch, note)))
        return 0;                                     /* OTHER = OFF: nothing (a note-off only ends what a note-on of
                                                       * that channel started on the selected track, before) */
    t = TSEL;
    if (on)
        midi_sel_set(ch, note, song.sel + 1u);
    else if (midi_sel_get(ch, note)) {
        t = &trk[(midi_sel_get(ch, note) - 1u) % NTRK];
        midi_sel_set(ch, note, 0);
    }
    return t;
}

/* (2.6) the pedal and the wheel of a channel: the track they act on is remembered (track + 1), so a pedal let up or a
 * wheel let go after another track got selected (a SEL channel, IN = CH n) still lets go of the one they held */
static uint8_t ch_sus[16], ch_bend[16];
static void sus_set(track_t *t, int on);
static void midi_pedal(uint32_t ch, track_t *t, uint32_t value)
{
    uint32_t was = ch_sus[ch & 15u];
    if (was && was - 1u != trk_index(t))
        sus_set(&trk[(was - 1u) % NTRK], 0);           /* (the pedal's other track: let go there) */
    ch_sus[ch & 15u] = (uint8_t)(value >= 64u ? trk_index(t) + 1u : 0u);
}
static void midi_bend(uint32_t ch, track_t *t, uint32_t d1, uint32_t d2)
{
    uint32_t was = ch_bend[ch & 15u];
    if (was && was - 1u != trk_index(t))
        trk[(was - 1u) % NTRK].bend = 0;                /* (the wheel's other track: back to its pitch, gliding) */
    t->bend = (int16_t)((int32_t)(d1 | d2 << 7) - 8192);
    ch_bend[ch & 15u] = (uint8_t)(trk_index(t) + 1u);
}

/* MIDI CCs set track parameters as a knob would (0..127 over the range, 64 the middle of a bipolar one), on the
 * track the channel plays (as the notes). After SLOOP 2.5 / Felucca 1.1.5's map, on the DX7: 5 GLIDE, 7 LEVEL,
 * 10 PAN, 71 RESO, 72 / 73 / 75 the REL / ATK / DEC macros, 74 CUT, 91 / 93 / 94 the reverb, chorus and delay
 * sends; 64 the sustain pedal (4.0). The drum track: 7, 91, 94 as GLO > DRUMS LVL, REV, DLY, and 10 PAN */
static const uint8_t MIDI_CC_MAP[][2] = {
    {5, P_GLIDE}, {7, P_LEVEL}, {10, P_PAN}, {71, P_E7}, {72, P_E4}, {73, P_E2}, {74, P_E6}, {75, P_E3},
    {91, P_REV}, {93, P_CHOR}, {94, P_DLY},
};
/* MIDI LEARN (SLOOP 2.6, after Felucca 1.5 by Leo Kuroshita, Discussion #170): up to ML_N CCs, each set to one
 * parameter of one track, whatever channel it comes on (a channel IN ignores stays ignored) and whichever track that
 * channel plays. Kept with the settings (project.c settings_write, after the SYN kits), an entry 16 bits: bits 0..6
 * the CC, 7..8 the track, 9..15 the parameter's code, 0 = an empty entry: a common parameter its P_* id + 1 (their
 * ids never move, new ones go before P_E0), an engine parameter ML_E0 + k for P_E0 + k (P_E0 moves when common ones
 * are added). A learned CC no longer reaches the standard map on any channel: the controller is the user's now. The
 * CCs with a meaning of their own (0 / 32 bank select, 1 mod wheel, 6 / 38 data entry, 11 expression, 64 sustain,
 * 96..101, 120..127 the channel modes) are never learned. The UI (ui_input.c ml_*) writes the entries with the
 * interrupts off; ml_arm / ml_heard hand it a CC heard while learning */
#define ML_N 16u
#define ML_E0 120u
_Static_assert(P_E0 < ML_E0 && P_COUNT - P_E0 == 8, "MIDI LEARN: a code per parameter");
static volatile uint16_t ml_tab[ML_N];
static volatile uint8_t ml_arm, ml_heard;  /* the UI waits for a CC; the CC that came meanwhile + 1 (0 none) */
static uint32_t ml_id(uint32_t code)        /* an entry's code -> P_* id, P_COUNT: none this firmware knows */
{
    return code >= ML_E0 ? (code - ML_E0 < 8u ? P_E0 + code - ML_E0 : P_COUNT) : code && code <= P_E0 ? code - 1u : P_COUNT;
}
static int ml_free_cc(uint32_t cc)
{
    return cc && cc < 120u && cc != 1u && cc != 6u && cc != 11u && cc != 32u && cc != 38u && cc != 64u &&
           (cc < 96u || cc > 101u);
}
/* parameter id of track t from a CC value, as a knob over its range */
/* the pedal on track t: up, its waiting notes end now */
static void sus_set(track_t *t, int on)
{
    uint32_t ti = trk_index(t) % NTRK, n;
    if (is_drum(t))
        return;
    sus_on[ti] = (uint8_t)(on != 0);
    if (on)
        return;
    for (n = 0; n < 128u; n++)
        if ((sus_wait[ti][n >> 5] >> (n & 31u)) & 1u) {
            sus_wait[ti][n >> 5] &= ~(1u << (n & 31u));
            input_off(t, n);
        }
}
static void midi_cc_set(track_t *t, uint32_t id, uint32_t value)
{
    const param_desc_t *d = is_drum(t) && id == P_E0 ? &DRUM_KIT_DESC
                            : id >= P_E0 ? &ENGINES[t->eng_req % NENGINES]->edit[(id - P_E0) & 7u] : &TP[id % P_COUNT];
    if (d->max > d->min)
        t->p[id % P_COUNT] = (int16_t)(d->min + ((int32_t)(value & 127u) * (d->max - d->min) + 63) / 127);
}
/* 1: cc is learned (set here) or taken by MIDI LEARN waiting for one */
static int __attribute__((noinline)) midi_learned(uint32_t cc, uint32_t value)
{
    uint32_t i, e, id, hit = 0;
    if (!ml_free_cc(cc)) {
        if (ml_arm && !ml_heard)
            ml_heard = (uint8_t)(cc + 1u);              /* (the UI says CC NOT LEARNABLE; the CC does its own job) */
        return 0;
    }
    if (ml_arm) {
        if (!ml_heard)
            ml_heard = (uint8_t)(cc + 1u);
        return 1;
    }
    for (i = 0; i < ML_N; i++) {
        e = ml_tab[i];
        if ((e >> 9) && (e & 127u) == cc) {
            if ((id = ml_id(e >> 9)) < P_COUNT)
                midi_cc_set(&trk[(e >> 7) & 3u], id, value);
            hit = 1;
        }
    }
    return hit;
}

static void __attribute__((noinline)) midi_cc(track_t *t, uint32_t cc, uint32_t value)
{
    const param_desc_t *d = 0;
    int16_t *slot = 0;
    uint32_t i, id = 0xFFFFu;
    if (midi_learned(cc, value))                       /* (a learned CC is the user's: not the map's) */
        return;
    if (cc == 64u) {                                   /* the sustain pedal (4.0, SLOOP 2.6): >= 64 down */
        sus_set(t, value >= 64u);
        return;
    }
    for (i = 0; i < NELEM(MIDI_CC_MAP); i++)
        if (MIDI_CC_MAP[i][0] == cc)
            id = MIDI_CC_MAP[i][1];
    if (id == 0xFFFFu)
        return;
    if (is_drum(t)) {
        if (id == P_LEVEL || id == P_REV || id == P_DLY) {
            id = id == P_LEVEL ? G_DRLVL : id == P_REV ? G_DRREV : G_DRDLY;
            d = &GP[id];
            slot = &song.g[id];
        } else if (id == P_PAN) {
            d = &TP[id];
            slot = &t->p[id];
        }
    } else {
        d = id >= P_E0 && id <= P_E7 ? &ENGINES[t->engine % NENGINES]->edit[id - P_E0] : &TP[id];
        slot = &t->p[id];
    }
    if (!d || d->max <= d->min)
        return;
    *slot = (int16_t)(d->min + ((int32_t)value * (d->max - d->min) + 63) / 127);
}

/* MIDI clock in (GLO > SYSTEM > SYNC = USB or TRS; after Felucca 1.0's midi_clock.c, from contributions by
 * ChanceTheMaker and keremimo): 24 pulses a beat. While the clock runs, the sequencer advances by the
 * pulses (a pulse = BEAT_U / 24 units), interpolated up to the next one from the last interval but never
 * past it, so it follows the master's tempo changes and cannot drift; BPM shows the master's tempo
 * (the slicer, delay and arp follow it). START restarts from the top, CONTINUE carries on where it
 * stopped, STOP stops. With no pulse for 0.5 s, the internal tempo takes over (PLAY works as ever). */
#define MCLK_PULSE_U (BEAT_U / 24u)
static struct {
    uint32_t pos, done;          /* units: the master's position (pulses since START), ours */
    uint32_t last_ms, iv_ms;     /* the last pulse, the interval between pulses (smoothed) */
    uint32_t beat_ms;            /* when pulse 0 of the last 24 came: the tempo */
    uint8_t have, n24;           /* a pulse since START; pulses towards the next tempo reading */
    uint8_t alive;               /* pulses are coming (from the SYNC source) */
} mclk;

static int mclk_on(void)                          /* the clock drives the sequencer */
{
    return song.g[G_SYNC] && mclk.alive && fm1_ms - mclk.last_ms < 500u;
}

static void mclk_event(uint32_t st, uint32_t src)  /* a realtime message; src 1 USB, 2 TRS */
{
    uint32_t now = fm1_ms;
    if (!song.g[G_SYNC] || src != (uint32_t)song.g[G_SYNC])
        return;
    if (st == 0xFAu || st == 0xFBu) {              /* START: from the top; CONTINUE: on from where it stopped */
        mclk.pos = mclk.done = 0;
        mclk.have = 0;
        if (st == 0xFAu)
            transport_req = 3;                     /* (not 1: the master counts, never a count-in; SLOOP 2.4) */
        else if (!song.playing)
            song.playing = 1;
        return;
    }
    if (st == 0xFCu) {                             /* STOP */
        transport_req = 2;
        return;
    }
    if (st != 0xF8u)
        return;
    if (mclk.alive && now - mclk.last_ms < 200u) { /* the interval, smoothed (a gap is not a tempo) */
        uint32_t iv = now - mclk.last_ms;
        mclk.iv_ms = mclk.iv_ms ? (mclk.iv_ms * 3u + iv + 2u) / 4u : iv;
    }
    if (!mclk.alive || now - mclk.last_ms >= 500u) {   /* (re)started: count a fresh beat */
        mclk.n24 = 0;
        mclk.beat_ms = now;
    } else if (++mclk.n24 == 24u) {                /* a beat: the tempo */
        uint32_t dt = now - mclk.beat_ms;
        mclk.n24 = 0;
        mclk.beat_ms = now;
        if (dt >= 250u && dt <= 1500u)             /* 40..240 BPM */
            song.g[G_BPM] = (int16_t)clamp((int32_t)((60000u + dt / 2u) / dt), 40, 240);
    }
    mclk.alive = 1;
    mclk.last_ms = now;
    if (song.playing || transport_req == 1u || transport_req == 3u) {   /* (a START queued with it: the next block starts) */
        if (mclk.have)
            mclk.pos += MCLK_PULSE_U;
        mclk.have = 1;                             /* the first pulse after START is the downbeat */
    }
}

static uint32_t mclk_adv(uint32_t n)               /* units to advance this block (mclk_on) */
{
    uint32_t el, off = 0, tgt, adv, cap = n * 2u * (uint32_t)song.g[G_BPM];
    if (!mclk.have)
        return 0;                                  /* START seen: wait for the downbeat */
    el = fm1_ms - mclk.last_ms;
    if (mclk.iv_ms) {
        if (el > mclk.iv_ms)
            el = mclk.iv_ms;
        off = el * MCLK_PULSE_U / mclk.iv_ms;      /* (<= 200 x 110250: fits 32 bits) */
        if (off >= MCLK_PULSE_U)
            off = MCLK_PULSE_U - 1u;
    }
    tgt = mclk.pos + off;
    adv = (int32_t)(tgt - mclk.done) > 0 ? tgt - mclk.done : 0u;
    if (adv > cap)
        adv = cap;                                 /* behind: catch up at twice the tempo, no burst */
    mclk.done += adv;
    return adv;
}

/* 2.7, GLO > MIDI 3 > CLK OUT: SLOOP the clock master on USB. The MIDI clock, 24 pulses a beat at the tempo, sent
 * stopped too (the other gear knows the tempo before PLAY); START as the sequencer starts (after a count-in: when it
 * starts), with the pulse of its downbeat; STOP when it stops (CLK OUT or SYNC changed while playing: STOP too). Only
 * while SYNC is INT: a FM-1 that follows a clock never sends one back (no loop through a computer). A pulse goes out
 * in the block it falls in: < 1 ms late */
static uint32_t mco_u;                            /* units since the last pulse */
static uint8_t mco_play;                          /* the other gear was told START */
static void clkout_block(uint32_t n)
{
    if (!song.g[G_CLKOUT] || song.g[G_SYNC]) {
        if (mco_play)
            midi_out_event(0x0Fu | 0xFCu << 8);       /* STOP */
        mco_play = 0;
        mco_u = 0;
        return;
    }
    if (song.playing && !mco_play) {
        midi_out_event(0x0Fu | 0xFAu << 8);           /* START, then the downbeat's pulse */
        midi_out_event(0x0Fu | 0xF8u << 8);
        mco_u = 0;
    } else if (!song.playing && mco_play) {
        midi_out_event(0x0Fu | 0xFCu << 8);           /* STOP */
    }
    mco_play = (uint8_t)(song.playing != 0);
    mco_u += n * (uint32_t)song.g[G_BPM];
    while (mco_u >= MCLK_PULSE_U) {
        mco_u -= MCLK_PULSE_U;
        midi_out_event(0x0Fu | 0xF8u << 8);           /* a pulse */
    }
}

/* everything that happens between two rendered blocks: transport, input, the steps of every
 * track at the clock, the click, the rolls and the arps; then the clock moves on by n samples */
static void events_block(uint32_t n)
{
    uint32_t i, pr, adv;
    if (mo_any && !song.g[G_MIDI]) {            /* MIDI = KEYS again: end what the sequencer had sent */
        seq_out_all_off();
        mo_any = 0;
    }
    if (transport_req == 1u || transport_req == 3u) {
        uint32_t ext = transport_req == 3u;         /* a MIDI START: the master counts; cut a count-in short (SLOOP 2.4) */
        transport_req = 0;
        if (ext && ci_on)
            ci_on = 0;
        if (ft_on) {
            ft_close();                             /* (PLAY from elsewhere: the editor) */
        } else if (ci_on) {
            ci_on = 0;                              /* PLAY again during the count-in: back to armed */
        } else if (!ext && rec_wait && rec_count && !song.playing && !mclk_on() &&
                   !(project_empty() && !rec_tempo)) {
            ci_on = 1;                              /* COUNT: one bar of clicks first (below) */
            ci_u = 0;
            ci_beat = 0;
            drum_on(77u, 120u);
        } else {
            seq_start();
            if (rec_wait && song.playing)
                rec_begin();                        /* PLAY while armed: record from the top */
        }
    } else if (transport_req == 2u) {
        seq_stop();
        transport_req = 0;
        song.rec = 0;                               /* STOP ends the take (and the wait) */
        rec_wait = 0;
        if (ft_on) {
            ft_on = 0;                              /* a free take: dropped */
            ft_bars = 0xFF;
        }
    }
    if (ci_on) {                                    /* the count-in: 4 beats at the tempo, then go */
        if (!rec_wait || song.playing) {
            ci_on = 0;                              /* (REC cancelled it, or it started some other way) */
        } else {
            uint32_t b;
            ci_u += n * (uint32_t)song.g[G_BPM];
            b = ci_u / BEAT_U;
            if (b >= 4u) {
                ci_on = 0;
                seq_start();
                if (song.playing)
                    rec_begin();
            } else if (b != ci_beat) {
                ci_beat = (uint8_t)b;
                drum_on(76u, 72u);
            }
        }
    }
    if (rec_wait && song.playing)
        rec_begin();                                /* started some other way: record now */
    adv = mclk_on() && song.playing ? mclk_adv(n) : n * (uint32_t)song.g[G_BPM];   /* (after a START) */
    clkout_block(n);                                  /* (2.7) CLK OUT */
    ft_block();
#if FELUCCA_ARRANGER
    if (song.playing && arrangement_clock.running) {
        int scene = arr_next(&arrangement_clock, &arrangement, FS);
        if (scene == ARR_DONE) {
            arrangement_clock.running = 1;          /* (arr_next cleared it: seq_stop brings the loop back) */
            seq_stop();
        }
        else if (scene >= 0) {
            arrangement_apply((uint32_t)scene);
            seq_reset_tracks(arrangement_clock.phase);   /* (the remainder: exactly on the bar) */
        }
    } else if (song.playing) {
        live_block();
    }
#endif
    {   /* GLO > SYSTEM > IN changed to or from CH n, or a channel of GLO > MIDI / DRUMS (2.6): what the other routing
         * started is released (its note-offs would go elsewhere now: stuck notes), the pedals and wheels too */
        static uint8_t route_was;
        static uint32_t chans_was = 0xFFFFFFFFu;
        uint32_t rt = (uint32_t)song.g[G_ROUTE];
        uint32_t chans = (uint32_t)(song.g[G_CH1] & 31) | (uint32_t)(song.g[G_CH2] & 31) << 5 |
                         (uint32_t)(song.g[G_CH3] & 31) << 10 | (uint32_t)(song.g[G_CHOTH] & 1) << 15 |
                         (uint32_t)(song.g[G_DRCH] & 31) << 16 | (uint32_t)(song.g[G_FXCH] & 31) << 21;   /* (2.7: FX CH too:
                                                       * its channel's notes go to the effects from then on) */
        if ((rt != route_was && (rt >= 2u || route_was >= 2u)) || (chans != chans_was && chans_was != 0xFFFFFFFFu)) {
            panic_req = (uint8_t)((1u << NTRK) - 1u);
            seq_out_all_off();                        /* (MIDI OUT: the notes on end on the channels they went out on) */
            memset(midi_sel_on, 0, sizeof midi_sel_on);
            memset(ch_sus, 0, sizeof ch_sus);
            memset(ch_bend, 0, sizeof ch_bend);
            for (i = 0; i < NTRK; i++)
                trk[i].bend = 0;
        }
        route_was = (uint8_t)rt;
        chans_was = chans;
    }
    pr = panic_req;
    panic_req = 0;
    {
        uint32_t ak = arp_keep_req;
        arp_keep_req = 0;
        if (pr == (1u << NTRK) - 1u)
            ak = 0;                                   /* (all of it: a project, NEW: no arp goes on) */
        arp_keep_req = (uint8_t)ak;                   /* (read below, cleared after) */
    }
    if (pr)
        mi_pen[0] = mi_pen[1] = mi_pen[2] = mi_pen[3] = 0;   /* (the MIDI pen's notes held: never stuck either) */
    if (pr == (1u << NTRK) - 1u) {                    /* (2.6) all of it (a project, NEW, the MIDI routing): the
                                                       * wheels back to the middle too */
        memset(ch_bend, 0, sizeof ch_bend);
        memset(ch_sus, 0, sizeof ch_sus);
        for (i = 0; i < NTRK; i++)
            trk[i].bend = 0;
    }
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        if ((pr >> i) & 1u) {
            kh_release(t);                            /* (2.7) KBD HOLD: what it held ends */
            if (t->pold) {                            /* (2.6) a preset loaded: the notes keep sounding as they are
                                                       * and fade out with the old sound (voice.c sound_change):
                                                       * no release cut short (an organ's key-off) under the fade */
                t->nmono = 0, t->mono_note = 0, t->xp_n = 0;
            } else {
                trk_all_off(t);
            }
            sus_wait[i][0] = sus_wait[i][1] = sus_wait[i][2] = sus_wait[i][3] = 0;   /* (what the pedal held: gone too, */
            sus_on[i] = 0;                                                         /* and the pedal: never stuck) */
            if (!((arp_keep_req >> i) & 1u)) {   /* (2.7: a sound loaded under a running arp:
                                                       * it keeps its notes, ui.c apply_preset_to) */
                t->nheld = 0;
                t->arp_phys = 0;
                t->arp_note = t->arp_ch[0] = t->arp_ch[1] = t->arp_ch[2] = 0;
            }
        }
        if (i < NPART)
            engine_block(t);                          /* engine switch: fade, then switch (voice.c) */
        /* ARP turned off, or HOLD released with no key down: drop the latched chord */
        if ((t->armp && !ARP_RUNS(t)) || (t->aholdp && !t->p[P_AHOLD] && !t->arp_phys)) {
            t->nheld = 0;
            if (!ARP_RUNS(t))
                t->arp_phys = 0;
            arp_end(t);
        }
        if (kh_any(t) && !kh_on(t))
            kh_release(t);                            /* (2.7) KBD HOLD off (HOLD OFF, ARP on): the held notes end */
        t->armp = (int16_t)ARP_RUNS(t);
        t->aholdp = t->p[P_AHOLD];
    }
    if (pr)
        arp_keep_req = 0;
    keyboard_block();
    while (mi_r != mi_w) {                            /* USB-MIDI (and TRS) in */
        uint32_t pkt = midi_in_q[mi_r % MQ], st = (pkt >> 8) & 0xF0u, ch = (pkt >> 8) & 0x0Fu;
        uint32_t d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        track_t *t;
        mi_r++;
        if ((pkt & 15u) == 0xFu) {                    /* clock / transport: cable 0 USB, 1 TRS */
            mclk_event((pkt >> 8) & 0xFFu, ((pkt >> 4) & 15u) ? 2u : 1u);
            continue;
        }
        if (song.g[G_RMCH] && ch == (uint32_t)song.g[G_RMCH] - 1u && st == 0xB0u && d1 >= REMOTE_CC0 &&
            d1 < REMOTE_CC0 + 14u) {                  /* 2.7: REMOTE (GLO > MIDI 3): the buttons over MIDI */
            remote_cc(d1 - REMOTE_CC0, d2);
            continue;
        }
        if (song.g[G_FXCH] && ch == (uint32_t)song.g[G_FXCH] - 1u && song.g[G_ROUTE] != 1 &&
            (st == 0x90u || st == 0x80u)) {           /* 2.7: the punch-in FX's channel (GLO > MIDI 3), before IN = CH n */
            midi_fx(d1, st == 0x90u && d2);
            continue;
        }
        if (song.g[G_ROUTE] >= 2) {                   /* 2.6: IN = CH n: that channel only, on the selected
                                                       * track (notes and CCs); every other channel ignored */
            if (ch != (uint32_t)song.g[G_ROUTE] - 2u)
                continue;
            if (st == 0xB0u) {
                if (d1 == 64u)
                    midi_pedal(ch, TSEL, d2);
                midi_cc(TSEL, d1, d2);
                continue;
            }
            if (st == 0xE0u) {                        /* pitch bend (2.6) */
                if (!is_drum(TSEL))
                    midi_bend(ch, TSEL, d1, d2);
                continue;
            }
            if (st == 0xC0u) {                        /* Program Change (2.6): the selected track's sound */
                midi_pc[song.sel % NTRK] = (uint8_t)(d1 + 1u);
                continue;
            }
            if (st != 0x90u && st != 0x80u)
                continue;
            if (st == 0x90u && d2) {
                midi_sel_set(ch, d1, song.sel + 1u);
                t = TSEL;
            } else {                                  /* the note-off: the track its note-on went to */
                t = midi_sel_get(ch, d1) ? &trk[(midi_sel_get(ch, d1) - 1u) % NTRK] : TSEL;
                midi_sel_set(ch, d1, 0);
            }
        } else {
            if (st == 0xB0u) {                            /* a CC (IN = CLOCK: none) */
                if (!song.g[G_ROUTE] && (t = midi_track(ch)) != 0) {
                    if (d1 == 64u)
                        midi_pedal(ch, t, d2);
                    midi_cc(t, d1, d2);
                }
                else if (!song.g[G_ROUTE])
                    (void)midi_learned(d1, d2);           /* (OTHER = OFF: a learned CC still works, on any channel) */
                continue;
            }
            if (st == 0xC0u) {                            /* Program Change (2.6): the sound of the track its channel
                                                           * plays (IN = CLOCK, OTHER = OFF and nobody's channel: none) */
                if (!song.g[G_ROUTE] && (t = midi_track(ch)) != 0)
                    midi_pc[trk_index(t) % NTRK] = (uint8_t)(d1 + 1u);
                continue;
            }
            if (st == 0xE0u) {                            /* pitch bend (2.6): the track its channel plays (IN = CLOCK:
                                                           * none) */
                if (!song.g[G_ROUTE] && (t = midi_track(ch)) != 0 && !is_drum(t))
                    midi_bend(ch, t, d1, d2);
                continue;
            }
            if (st != 0x90u && st != 0x80u)
                continue;
            if (song.g[G_ROUTE] && st == 0x90u && d2)
                continue;                                 /* GLO > SYSTEM > IN = CLOCK: no notes (the note-offs still
                                                           * end what was held when it was set) */
            t = midi_route(ch, d1, st == 0x90u && d2);
            if (!t)
                continue;                                 /* (GLO > MIDI > OTHER = OFF: a channel no track has) */
        }
        if (is_drum(t)) {
            if (st == 0x90u && d2)
                drum_input(midi_drum_lane(d1), vel_lvl(d2), 0, 1);
        } else if (st == 0x90u && d2) {
            if (t == TSEL) {                          /* the SEQ layer's pen: this note, or the chord held (2.6) */
                if (!(mi_pen[0] | mi_pen[1] | mi_pen[2] | mi_pen[3]) || pen_n >= 4u)
                    pen_n = 0;
                pen_note[pen_n++] = (uint8_t)d1;
                mi_pen[d1 >> 5] |= 1u << (d1 & 31u);
            }
            step_in_push(d1 | 0x80u | trk_index(t) << 8);
            input_on(t, d1, d2);
        } else {
            mi_pen[d1 >> 5] &= ~(1u << (d1 & 31u));
            step_in_push(d1 | trk_index(t) << 8);
            input_off(t, d1);
        }
    }
    for (i = 0; i < NTRK; i++)                        /* OMNI tracks first: a chord step sets the */
        if (trk[i].p[P_AMODE] == AM_OMNI)            /* chord FLW tracks play in the same block */
            seq_tick(&trk[i], adv);
    for (i = 0; i < NTRK; i++)
        if (trk[i].p[P_AMODE] != AM_OMNI)
            seq_tick(&trk[i], adv);
    click_tick();
    roll_block(adv);
    for (i = 0; i < NPART; i++)
        arp_tick(&trk[i], adv);
    if (song.playing) {
        song.tick++;
        clk_pos += adv;
        while (clk_pos >= BEAT_U) {
            clk_pos -= BEAT_U;
            clk_beat++;
        }
#if FELUCCA_ARRANGER
        arr_elapse(&arrangement_clock, adv, 1u);   /* (units: n x BPM, or the MIDI clock) */
#endif
    }
}
