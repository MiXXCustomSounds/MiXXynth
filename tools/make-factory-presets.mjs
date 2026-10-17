// The factory bank's generator.
//
// The XMLs this writes into `presets/` ARE the bank and are committed; this script is how
// they were made, and how they are extended or regenerated. It exists because thirty patches
// written by hand is thirty chances to mistype a parameter id, and because the interesting
// part of each patch is a dozen numbers rather than four hundred.
//
//   node tools/make-factory-presets.mjs
//
// Every file it writes is a PARTIAL state: only what the patch changes. The loader lays it
// over the init patch, so a patch that omits a parameter gets that parameter's default. See
// docs/PLAN-M7.md. MiXXynthPatchAudit checks every id and every value here against the real
// parameter list, so a wrong id fails a test rather than sounding wrong.

import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

// ---- the rack's types, in FxType order ------------------------------------
const FX_TYPE = {
  WeirdVerb: 1, Convolution: 2, JunoChorus: 3, Distortion: 4, PingPongDelay: 5,
  Flanger: 6, Phaser: 7, Compressor: 8, Limiter: 9, Multiband: 10, GraphicEq: 11,
};

// Each module's controls, in descriptor order. Index + 1 is the `pN` in the parameter id.
const FX_PARAMS = {
  WeirdVerb: ['Mode', 'Size', 'Decay', 'Bass', 'Damping', 'Pre-Delay', 'Attack', 'Diffusion',
              'Mod', 'Rate', 'Low Cut', 'High Cut', 'Width', 'Duck', 'Shimmer', 'Mix', 'Freeze'],
  JunoChorus: ['Mode', 'Rate', 'Depth', 'Tone', 'Mix'],
  Distortion: ['Mode', 'Drive', 'Bias', 'Tone', 'Low Cut', 'Output', 'Oversample', 'Mix'],
  PingPongDelay: ['Sync', 'Time L', 'Time R', 'Feedback', 'Ping-Pong', 'Damping', 'Mod',
                  'Drift', 'Width', 'Mix'],
  Flanger: ['Rate', 'Depth', 'Feedback', 'Polarity', 'Delay', 'Stereo', 'Mix'],
  Phaser: ['Stages', 'Rate', 'Depth', 'Feedback', 'Centre', 'Stereo', 'Stage Offset', 'Mix'],
  Compressor: ['Detector', 'Threshold', 'Ratio', 'Attack', 'Release', 'Knee', 'Makeup', 'Mix'],
  Limiter: ['Threshold', 'Release', 'Soft Clip', 'Mix'],
  Multiband: ['Split Low', 'Split High', 'Downward', 'Upward', 'Time', 'Depth'],
};

// ---- the modulation sources and destinations, in enum order ----------------
const SRC = { lfo1: 1, lfo2: 2, lfo3: 3, lfo4: 4, env1: 9, env2: 10, macro1: 17, macro2: 18,
              velocity: 21, modWheel: 22, aftertouch: 23, keyTrack: 24 };

const DST = { fmIndex: 1, op1ratio: 2, op2ratio: 3, op3ratio: 4, op4ratio: 5, op5ratio: 6,
              op6ratio: 7, op1level: 8, op2level: 9, op3level: 10, op4level: 11, op5level: 12,
              op6level: 13, master: 14, vaLevel: 15, vaDetune: 16, vaShape: 17,
              filterCutoff: 18, filterReso: 19, filterDrive: 20,
              op1fltCutoff: 21, op2fltCutoff: 22, op3fltCutoff: 23, op4fltCutoff: 24,
              op5fltCutoff: 25, op6fltCutoff: 26 };


// ---- the expansion --------------------------------------------------------
// A patch is a dozen numbers; this turns them into the four hundred the state holds. Every
// operator is written even when it does nothing, because a partial state that omits an
// operator would inherit the init patch's level of 0 anyway - writing it says so out loud.
function expand (spec) {
  const p = {};

  const env = spec.env ?? {};
  const wave = spec.wave ?? 0.0;

  for (let i = 1; i <= 6; i++) {
    const op = (spec.ops ?? {})[i] ?? {};

    p[`op${i}on`] = 1.0;
    p[`op${i}ratio`] = op.ratio ?? 1.0;
    p[`op${i}level`] = op.level ?? 0.0;
    p[`op${i}feedback`] = op.feedback ?? 0.0;
    p[`op${i}wave`] = op.wave ?? wave;
    p[`op${i}attack`] = op.attack ?? env.attack ?? 0.002;
    p[`op${i}decay`] = op.decay ?? env.decay ?? 0.8;
    p[`op${i}sustain`] = op.sustain ?? env.sustain ?? 0.7;
    p[`op${i}release`] = op.release ?? env.release ?? 0.3;
    p[`carrier${i}`] = (spec.carriers ?? [1]).includes (i) ? 1.0 : 0.0;
  }

  // The routing. Every cell is written, so a patch cannot inherit a routing it did not ask
  // for - the loader lays a preset over the init patch, and init has every cell at zero.
  for (let src = 1; src <= 6; src++)
    for (let dst = 1; dst <= 6; dst++)
      if (src !== dst)
        p[`mtx${src}${dst}`] = (spec.route ?? {})[`${src}>${dst}`] ?? 0.0;

  p.fmIndex = spec.index ?? 0.3;
  p.masterGain = spec.master ?? 0.6;

  // The VA layer.
  const va = spec.va ?? {};
  p.vaLevel = va.level ?? 0.0;
  p.vaShape = va.shape ?? 1.0 / 3.0;
  p.vaOctave = va.octave ?? 0.0;
  p.vaDetune = va.detune ?? 0.0;
  p.vaSubLevel = va.sub ?? 0.0;
  p.vaNoiseLevel = va.noise ?? 0.0;
  p.vaAttack = va.attack ?? 0.008;
  p.vaDecay = va.decay ?? 0.0;
  p.vaSustain = va.sustain ?? 1.0;
  p.vaRelease = va.release ?? 0.05;

  // The filter.
  const f = spec.filter ?? {};
  p.filterOn = 1.0;
  p.filterType = f.type ?? 0.0;
  p.filterCutoff = f.cutoff ?? 20000.0;
  p.filterReso = f.reso ?? 0.2;
  p.filterDrive = f.drive ?? 0.0;

  // A per-operator filter, when the patch asks for one.
  for (const [n, pf] of Object.entries (spec.opFilter ?? {})) {
    p[`op${n}fltOn`] = 1.0;
    p[`op${n}fltType`] = pf.type ?? 0.0;
    p[`op${n}fltCutoff`] = pf.cutoff ?? 4000.0;
    p[`op${n}fltReso`] = pf.reso ?? 0.2;
    p[`op${n}fltDrive`] = pf.drive ?? 0.0;
    p[`op${n}fltAttack`] = pf.attack ?? 0.005;
    p[`op${n}fltDecay`] = pf.decay ?? 0.3;
    p[`op${n}fltSustain`] = pf.sustain ?? 0.0;
    p[`op${n}fltRelease`] = pf.release ?? 0.2;
    p[`op${n}fltRoute`] = pf.route ?? 2.0;
  }

  // The rack. A slot's controls are written out because the generic parameters default to
  // zero, and zero is not the module's default: a module whose type was set and whose controls
  // were not would load with every control at the bottom of its range.
  (spec.fx ?? []).forEach (([name, values = {}], i) => {
    const slot = i + 1;
    const names = FX_PARAMS[name];

    if (names === undefined)
      throw new Error (`unknown module ${name}`);

    p[`fx${slot}type`] = FX_TYPE[name];
    p[`fx${slot}bypass`] = 0.0;

    for (const [control, value] of Object.entries (values)) {
      const index = names.indexOf (control);

      if (index < 0)
        throw new Error (`${name} has no control ${control}`);

      p[`fx${slot}p${index + 1}`] = value;
    }
  });

  // The modulation matrix.
  (spec.mod ?? []).forEach ((m, i) => {
    const slot = i + 1;

    if (SRC[m.src] === undefined || DST[m.dst] === undefined)
      throw new Error (`unknown source or destination in ${spec.name}`);

    p[`mod${slot}src`] = SRC[m.src];
    p[`mod${slot}dst`] = DST[m.dst];
    p[`mod${slot}depth`] = m.depth;
  });

  return p;
}



// ---- the modules' usable starting points ---------------------------------
// A module whose controls are left unwritten sits at zero, which is not the module's default
// and is rarely usable. These are the settings a patch gets when it names a module and does
// not say more than that. Choice controls are deliberately absent: their index 0 is a valid
// choice, so a patch that does not care about a mode gets the module's first one.
function fx (name, over = {}) {
  const base = {
    WeirdVerb: { Size: 0.5, Decay: 0.55, Damping: 0.35, Diffusion: 0.7, Mix: 0.25 },
    JunoChorus: { Rate: 1.0, Depth: 1.0, Tone: 0.6, Mix: 0.5 },
    Distortion: { Drive: 0.45, Tone: 0.5, Output: 0.6, Mix: 0.5 },
    PingPongDelay: { 'Time L': 0.35, 'Time R': 0.5, Feedback: 0.35, 'Ping-Pong': 0.8,
                     Damping: 0.4, Width: 0.8, Mix: 0.28 },
    Flanger: { Rate: 0.25, Depth: 0.6, Feedback: 0.45, Delay: 0.2, Stereo: 0.7, Mix: 0.5 },
    Phaser: { Rate: 0.2, Depth: 0.6, Feedback: 0.4, Centre: 0.5, Stereo: 0.6, Mix: 0.5 },
    Compressor: { Threshold: 0.5, Ratio: 0.35, Attack: 0.15, Release: 0.4, Makeup: 0.55,
                  Mix: 0.9 },
    Multiband: { 'Split Low': 0.25, 'Split High': 0.7, Downward: 0.6, Upward: 0.3, Time: 0.4,
                 Depth: 0.6 },
  }[name];

  if (base === undefined)
    throw new Error (`no starting point for ${name}`);

  return [name, { ...base, ...over }];
}

// ---- the bank -------------------------------------------------------------
// A placeholder bank: thirty patches whose job is to show the engine's range and to be
// replaced by the author's own. Each one is deliberately a different corner of the synth - a
// two-operator bass, a five-modulator bell, a VA pad - so the bank is also a tour of it.
const PATCHES = [
  // ---- bass ----
  { cat: 'Bass', name: 'Sub Growl', index: 0.42, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.85 }, 2: { ratio: 2, level: 0.75 }, 3: { ratio: 3, level: 0.3 } },
    route: { '2>1': 0.6, '3>2': 0.35 },
    env: { attack: 0.002, decay: 0.45, sustain: 0.6, release: 0.25 },
    filter: { cutoff: 900, reso: 0.35, drive: 0.25 },
    fx: [fx('Distortion', { Drive: 0.5, Mix: 0.4 }), fx('Compressor')],
    mod: [{ src: 'lfo1', dst: 'filterCutoff', depth: 0.3 }] },

  { cat: 'Bass', name: 'Reese Wall', index: 0.3, carriers: [1, 2],
    ops: { 1: { ratio: 1, level: 0.7 }, 2: { ratio: 1, level: 0.7 },
           3: { ratio: 1.5, level: 0.5 } },
    route: { '3>1': 0.5, '3>2': 0.5 },
    env: { attack: 0.01, decay: 0.6, sustain: 0.8, release: 0.4 },
    va: { level: 0.35, shape: 0.35, sub: 0.45, octave: -1 },
    filter: { cutoff: 1600, reso: 0.25 },
    fx: [fx('Flanger', { Rate: 0.12, Feedback: 0.55, Mix: 0.4 }), fx('Compressor')] },

  { cat: 'Bass', name: 'Acid Line', index: 0.55, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.9 }, 2: { ratio: 2, level: 0.6 },
           3: { ratio: 4, level: 0.35, feedback: 0.2 } },
    route: { '2>1': 0.7, '3>2': 0.3 },
    env: { attack: 0.001, decay: 0.3, sustain: 0.5, release: 0.15 },
    filter: { cutoff: 700, reso: 0.72, drive: 0.35 },
    fx: [fx('Phaser', { Rate: 0.15, Feedback: 0.5, Mix: 0.45 })],
    mod: [{ src: 'env1', dst: 'filterCutoff', depth: 0.55 }] },

  { cat: 'Bass', name: 'FM Sub', index: 0.22, carriers: [1],
    ops: { 1: { ratio: 0.5, level: 0.9 }, 2: { ratio: 1, level: 0.55 } },
    route: { '2>1': 0.4 },
    env: { attack: 0.004, decay: 0.5, sustain: 0.75, release: 0.3 },
    va: { level: 0.25, shape: 0.5, sub: 0.5, octave: -1 },
    filter: { cutoff: 2500, reso: 0.15 },
    fx: [fx('Compressor', { Threshold: 0.45, Ratio: 0.5 })] },

  // ---- lead ----
  { cat: 'Lead', name: 'Glass Lead', index: 0.38, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.85 }, 2: { ratio: 3, level: 0.5 },
           3: { ratio: 7, level: 0.22 } },
    route: { '2>1': 0.55, '3>2': 0.3 },
    env: { attack: 0.006, decay: 0.7, sustain: 0.65, release: 0.5 },
    filter: { cutoff: 6000, reso: 0.3 },
    fx: [fx('JunoChorus'), fx('PingPongDelay', { Mix: 0.22 })] },

  { cat: 'Lead', name: 'Screamer', index: 0.85, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.8 }, 2: { ratio: 2, level: 0.9, feedback: 0.55 },
           3: { ratio: 5, level: 0.4 } },
    route: { '2>1': 0.9, '3>2': 0.5 },
    env: { attack: 0.003, decay: 0.5, sustain: 0.7, release: 0.3 },
    filter: { cutoff: 5000, reso: 0.4, drive: 0.4 },
    fx: [fx('Distortion', { Drive: 0.6, Mix: 0.55 }), fx('Multiband', { Downward: 0.7 })] },

  { cat: 'Lead', name: 'Soft Flute', index: 0.14, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.8 }, 2: { ratio: 2, level: 0.3 } },
    route: { '2>1': 0.25 },
    env: { attack: 0.09, decay: 0.8, sustain: 0.85, release: 0.45 },
    va: { level: 0.3, shape: 0.0, sub: 0.15 },
    filter: { cutoff: 4000, reso: 0.15 },
    fx: [fx('WeirdVerb', { Mix: 0.3 })],
    mod: [{ src: 'lfo1', dst: 'vaLevel', depth: 0.2 }] },

  { cat: 'Lead', name: 'Rip Saw', index: 0.35, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.5 }, 2: { ratio: 1.5, level: 0.45 } },
    route: { '2>1': 0.4 },
    env: { attack: 0.01, decay: 0.6, sustain: 0.75, release: 0.35 },
    va: { level: 0.6, shape: 0.72, sub: 0.2, detune: 8 },
    filter: { cutoff: 3500, reso: 0.35, drive: 0.3 },
    fx: [fx('Flanger', { Rate: 0.2, Mix: 0.35 }), fx('Distortion', { Drive: 0.35, Mix: 0.3 })] },

  // ---- keys ----
  { cat: 'Keys', name: 'Electric Piano', index: 0.3, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.8 }, 2: { ratio: 14, level: 0.22 },
           3: { ratio: 1, level: 0.35 } },
    route: { '2>1': 0.35, '3>1': 0.3 },
    env: { attack: 0.002, decay: 1.1, sustain: 0.35, release: 0.6 },
    filter: { cutoff: 5000, reso: 0.2 },
    fx: [fx('JunoChorus', { Mix: 0.4 }), fx('Compressor')],
    mod: [{ src: 'velocity', dst: 'fmIndex', depth: 0.45 }] },

  { cat: 'Keys', name: 'Glass Keys', index: 0.26, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.75 }, 2: { ratio: 3.5, level: 0.4 },
           3: { ratio: 7, level: 0.18 } },
    route: { '2>1': 0.45, '3>1': 0.25 },
    env: { attack: 0.004, decay: 1.4, sustain: 0.3, release: 1.2 },
    fx: [fx('PingPongDelay', { Mix: 0.25 }), fx('WeirdVerb', { Mix: 0.35 })] },

  { cat: 'Keys', name: 'Wurly', index: 0.4, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.85 }, 2: { ratio: 2, level: 0.5 } },
    route: { '2>1': 0.6 },
    env: { attack: 0.003, decay: 0.9, sustain: 0.4, release: 0.5 },
    filter: { cutoff: 3200, reso: 0.3 },
    fx: [fx('JunoChorus', { Mix: 0.45 })],
    mod: [{ src: 'lfo1', dst: 'op2level', depth: 0.3 }] },

  { cat: 'Keys', name: 'Toy Piano', index: 0.6, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.8 }, 2: { ratio: 5.5, level: 0.45 },
           3: { ratio: 11, level: 0.2 } },
    route: { '2>1': 0.6, '3>1': 0.3 },
    env: { attack: 0.001, decay: 0.55, sustain: 0.15, release: 0.4 },
    fx: [fx('WeirdVerb', { Size: 0.35, Mix: 0.3 })] },


  // ---- bell ----
  { cat: 'Bell', name: 'Glass Bell', index: 0.32, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.7 }, 2: { ratio: 3.5, level: 0.5 },
           3: { ratio: 7, level: 0.25 } },
    route: { '2>1': 0.5, '3>1': 0.3 },
    env: { attack: 0.001, decay: 2.4, sustain: 0.12, release: 2.5 },
    fx: [fx('WeirdVerb', { Size: 0.7, Decay: 0.75, Mix: 0.4 })] },

  { cat: 'Bell', name: 'Temple Bell', index: 0.55, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.6 }, 2: { ratio: 7, level: 0.55, feedback: 0.35 },
           3: { ratio: 11, level: 0.2 } },
    route: { '2>1': 0.7, '3>2': 0.3 },
    env: { attack: 0.001, decay: 3.2, sustain: 0.05, release: 3.5 },
    fx: [fx('WeirdVerb', { Size: 0.85, Decay: 0.85, Shimmer: 0.3, Mix: 0.45 })] },

  { cat: 'Bell', name: 'Ice Bell', index: 0.24, carriers: [1],
    ops: { 1: { ratio: 2, level: 0.65 }, 2: { ratio: 9, level: 0.4 },
           3: { ratio: 13, level: 0.22 } },
    route: { '2>1': 0.45, '3>1': 0.3 },
    env: { attack: 0.002, decay: 2.0, sustain: 0.2, release: 2.0 },
    fx: [fx('WeirdVerb', { Size: 0.75, Shimmer: 0.5, Mix: 0.45 }),
         fx('PingPongDelay', { Mix: 0.2 })] },

  { cat: 'Bell', name: 'Clock Chime', index: 0.45, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.7 }, 2: { ratio: 4, level: 0.5 },
           3: { ratio: 6, level: 0.3 }, 4: { ratio: 9, level: 0.18 } },
    route: { '2>1': 0.5, '3>2': 0.4, '4>3': 0.3 },
    env: { attack: 0.001, decay: 1.6, sustain: 0.0, release: 1.8 },
    fx: [fx('PingPongDelay', { 'Time L': 0.28, 'Time R': 0.42, Mix: 0.3 }),
         fx('WeirdVerb', { Mix: 0.3 })] },

  // ---- pluck ----
  { cat: 'Pluck', name: 'Nylon Pluck', index: 0.34, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.8 }, 2: { ratio: 2, level: 0.45 },
           3: { ratio: 6, level: 0.18 } },
    route: { '2>1': 0.5, '3>2': 0.3 },
    env: { attack: 0.001, decay: 0.5, sustain: 0.08, release: 0.35 },
    filter: { cutoff: 4500, reso: 0.25 },
    fx: [fx('WeirdVerb', { Size: 0.4, Mix: 0.3 }), fx('Compressor')] },

  { cat: 'Pluck', name: 'FM Pluck', index: 0.5, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.85 }, 2: { ratio: 3, level: 0.6 } },
    route: { '2>1': 0.75 },
    env: { attack: 0.001, decay: 0.35, sustain: 0.0, release: 0.25 },
    fx: [fx('PingPongDelay', { Mix: 0.25 })] },

  { cat: 'Pluck', name: 'Kalimba', index: 0.28, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.8 }, 2: { ratio: 4, level: 0.5 } },
    route: { '2>1': 0.55 },
    env: { attack: 0.001, decay: 0.7, sustain: 0.05, release: 0.5 },
    filter: { cutoff: 6000, reso: 0.2 },
    fx: [fx('WeirdVerb', { Size: 0.3, Mix: 0.25 })] },


  // ---- pad ----
  { cat: 'Pad', name: 'Warm Pad', index: 0.22, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.6 }, 2: { ratio: 2, level: 0.4 },
           3: { ratio: 3, level: 0.25 } },
    route: { '2>1': 0.4, '3>1': 0.25 },
    env: { attack: 0.9, decay: 2.0, sustain: 0.85, release: 2.5 },
    va: { level: 0.4, shape: 0.5, sub: 0.25, detune: 6 },
    filter: { cutoff: 3000, reso: 0.2 },
    fx: [fx('JunoChorus', { Mix: 0.5 }), fx('WeirdVerb', { Size: 0.75, Mix: 0.4 })] },

  { cat: 'Pad', name: 'Glass Pad', index: 0.3, carriers: [1, 4],
    ops: { 1: { ratio: 1, level: 0.5 }, 2: { ratio: 3, level: 0.35 },
           3: { ratio: 5, level: 0.2 }, 4: { ratio: 2, level: 0.4 } },
    route: { '2>1': 0.35, '3>1': 0.2, '3>4': 0.3 },
    env: { attack: 1.2, decay: 2.5, sustain: 0.9, release: 3.0 },
    filter: { cutoff: 4500, reso: 0.25 },
    fx: [fx('WeirdVerb', { Size: 0.85, Shimmer: 0.4, Mix: 0.45 }),
         fx('PingPongDelay', { Mix: 0.18 })] },

  { cat: 'Pad', name: 'Drift Pad', index: 0.26, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.55 }, 2: { ratio: 2, level: 0.35 } },
    route: { '2>1': 0.35 },
    env: { attack: 1.5, decay: 3.0, sustain: 0.9, release: 3.5 },
    va: { level: 0.45, shape: 0.68, detune: 12, sub: 0.2 },
    filter: { cutoff: 2200, reso: 0.3 },
    fx: [fx('Phaser', { Rate: 0.08, Feedback: 0.45, Mix: 0.4 }),
         fx('WeirdVerb', { Size: 0.8, Mix: 0.4 })],
    mod: [{ src: 'lfo1', dst: 'fmIndex', depth: 0.25 }] },

  { cat: 'Pad', name: 'Dark Pad', index: 0.35, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.6 }, 2: { ratio: 1.5, level: 0.4 },
           3: { ratio: 0.5, level: 0.3 } },
    route: { '2>1': 0.4, '3>1': 0.35 },
    env: { attack: 1.8, decay: 3.0, sustain: 0.85, release: 4.0 },
    va: { level: 0.3, shape: 0.25, sub: 0.35, octave: -1 },
    filter: { cutoff: 900, reso: 0.3 },
    fx: [fx('WeirdVerb', { Size: 0.9, Decay: 0.85, Mix: 0.45 })] },


  // ---- sequence ----
  { cat: 'Sequence', name: 'Stepped Bass', index: 0.5, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.85 }, 2: { ratio: 2, level: 0.6 },
           3: { ratio: 3, level: 0.3 } },
    route: { '2>1': 0.65, '3>2': 0.35 },
    env: { attack: 0.001, decay: 0.3, sustain: 0.45, release: 0.2 },
    filter: { cutoff: 1400, reso: 0.5 },
    fx: [fx('PingPongDelay', { 'Time L': 0.3, 'Time R': 0.45, Mix: 0.25 }), fx('Compressor')],
    mod: [{ src: 'lfo1', dst: 'op2level', depth: 0.4 }] },

  { cat: 'Sequence', name: 'Pulse Motion', index: 0.4, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.8 }, 2: { ratio: 2, level: 0.5 } },
    route: { '2>1': 0.55 },
    env: { attack: 0.002, decay: 0.4, sustain: 0.5, release: 0.25 },
    va: { level: 0.3, shape: 0.85, sub: 0.2 },
    filter: { cutoff: 1800, reso: 0.6 },
    fx: [fx('PingPongDelay', { Mix: 0.3 }), fx('Compressor')],
    mod: [{ src: 'lfo1', dst: 'filterCutoff', depth: 0.5 },
          { src: 'lfo2', dst: 'vaShape', depth: 0.3 }] },

  { cat: 'Sequence', name: 'Metal Steps', index: 0.7, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.75 }, 2: { ratio: 5, level: 0.6, feedback: 0.4 },
           3: { ratio: 9, level: 0.3 } },
    route: { '2>1': 0.7, '3>2': 0.4 },
    env: { attack: 0.001, decay: 0.45, sustain: 0.3, release: 0.4 },
    filter: { cutoff: 3000, reso: 0.4 },
    fx: [fx('Phaser', { Rate: 0.3, Mix: 0.4 }), fx('PingPongDelay', { Mix: 0.22 })],
    mod: [{ src: 'lfo1', dst: 'op2level', depth: 0.45 }] },

  { cat: 'Sequence', name: 'FM Arp', index: 0.42, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.85 }, 2: { ratio: 3, level: 0.5 } },
    route: { '2>1': 0.6 },
    env: { attack: 0.001, decay: 0.28, sustain: 0.0, release: 0.2 },
    filter: { cutoff: 5000, reso: 0.3 },
    fx: [fx('PingPongDelay', { Feedback: 0.45, Mix: 0.32 }), fx('Compressor')],
    mod: [{ src: 'lfo1', dst: 'fmIndex', depth: 0.35 }] },

  // ---- fx ----
  { cat: 'FX', name: 'Wind Tunnel', index: 0.2, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.4 }, 2: { ratio: 1, level: 0.3 } },
    route: { '2>1': 0.3 },
    env: { attack: 1.5, decay: 3.0, sustain: 0.9, release: 3.0 },
    va: { level: 0.2, noise: 0.55, shape: 0.5 },
    filter: { cutoff: 1200, reso: 0.75, type: 2 },
    fx: [fx('WeirdVerb', { Size: 0.9, Decay: 0.9, Mix: 0.5 }),
         fx('Phaser', { Rate: 0.06, Feedback: 0.6, Mix: 0.5 })],
    mod: [{ src: 'lfo1', dst: 'filterCutoff', depth: 0.6 }] },

  { cat: 'FX', name: 'Broken Radio', index: 0.6, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.7 }, 2: { ratio: 7, level: 0.5, feedback: 0.5 },
           3: { ratio: 13, level: 0.3 } },
    route: { '2>1': 0.8, '3>2': 0.5 },
    env: { attack: 0.01, decay: 1.2, sustain: 0.6, release: 0.6 },
    va: { level: 0.25, noise: 0.35 },
    filter: { cutoff: 2000, reso: 0.6, type: 2, drive: 0.5 },
    fx: [fx('Distortion', { Drive: 0.75, Mix: 0.7 }),
         fx('WeirdVerb', { Size: 0.4, Mix: 0.3 })],
    mod: [{ src: 'lfo1', dst: 'op2level', depth: 0.5 },
          { src: 'lfo2', dst: 'fmIndex', depth: 0.3 }] },

  { cat: 'FX', name: 'Riser', index: 0.45, carriers: [1],
    ops: { 1: { ratio: 1, level: 0.7 }, 2: { ratio: 2, level: 0.5 },
           3: { ratio: 4, level: 0.35 } },
    route: { '2>1': 0.55, '3>2': 0.4 },
    env: { attack: 1.9, decay: 4.0, sustain: 1.0, release: 2.0 },
    va: { level: 0.3, noise: 0.25 },
    filter: { cutoff: 600, reso: 0.5 },
    fx: [fx('WeirdVerb', { Size: 0.9, Shimmer: 0.6, Mix: 0.5 }),
         fx('PingPongDelay', { Feedback: 0.5, Mix: 0.3 })],
    mod: [{ src: 'env1', dst: 'filterCutoff', depth: 0.8 },
          { src: 'env1', dst: 'fmIndex', depth: 0.4 }] },
];

// ---- the ranges, so a mistake fails here rather than at load -----------------
// A patch that writes 4.0 into a control whose range ends at 2.0 loads as 2.0 and says
// nothing, which is how a bank acquires patches that are not what their author meant. This
// knows the ranges of everything the generator writes; a value outside one is an error.
function rangeFor (id) {
  // Lower-cased once, because the ids are camel case and a suffix match on "Attack" that only
  // looked for "attack" is how vaAttack slipped through the first time.
  const k = id.toLowerCase ();

  if (k.endsWith ('on') || k.endsWith ('bypass') || k.startsWith ('carrier')) return [0, 1];
  if (k === 'fmindex') return [0, 2];
  if (k === 'mastergain') return [0, 1];
  if (k.startsWith ('mtx')) return [0, 1];
  if (k.endsWith ('ratio')) return [0, 16];
  if (k.endsWith ('level')) return [0, 1];
  if (k.endsWith ('feedback')) return [0, 0.95];
  if (k.endsWith ('wave')) return [0, 1];

  // A per-operator FILTER's envelope runs to eight seconds; an operator's own envelope and the
  // VA's are the narrow ones. Checked first, because "op1fltAttack" ends with both.
  if (k.endsWith ('fltattack')) return [0, 8];
  if (k.endsWith ('fltdecay')) return [0, 8];
  if (k.endsWith ('fltsustain')) return [0, 1];
  if (k.endsWith ('fltrelease')) return [0, 8];

  if (k.endsWith ('attack')) return [0, 2];
  if (k.endsWith ('decay')) return [0, 4];
  if (k.endsWith ('sustain')) return [0, 1];
  if (k.endsWith ('release')) return [0, 8];

  if (k.endsWith ('flttype') || k === 'filtertype' || k.endsWith ('fltroute')) return [0, 2];
  if (k.endsWith ('fltcutoff') || k === 'filtercutoff') return [20, 20000];
  if (k.endsWith ('fltreso') || k === 'filterreso') return [0, 0.98];
  if (k.endsWith ('fltdrive') || k === 'filterdrive') return [0, 1];
  if (k === 'vaoctave') return [-2, 2];
  if (k === 'vadetune') return [-50, 50];
  if (k === 'vashape') return [0, 1];

  if (/^fx\d+type$/.test (k)) return [0, 11];
  if (/^fx\d+p\d+$/.test (k)) return [0, 1];
  if (/^mod\d+src$/.test (k)) return [0, 25];
  if (/^mod\d+dst$/.test (k)) return [0, 68];
  if (/^mod\d+depth$/.test (k)) return [-1, 1];

  throw new Error (`no range known for ${id}`);
}

// ---- the writer -----------------------------------------------------------
// One PARAM child per line, sorted, so a diff of the bank reads as a change to a patch rather
// than as a wall of reflowed text.
//
// The shape is APVTS's own: a parameter is a CHILD node carrying `id` and `value`, not a
// property of the root. A file written the other way parses, loads, and changes nothing -
// which is the mistake this format exists to avoid, and the patch audit checks the round trip.
function toXml (spec) {
  const p = expand (spec);

  const lines = Object.keys (p).sort ().map ((k) => {
    const v = p[k];

    if (typeof v !== 'number' || !Number.isFinite (v))
      throw new Error (`${spec.name}: ${k} is not a number`);

    const [low, high] = rangeFor (k);

    if (v < low || v > high)
      throw new Error (`${spec.name}: ${k} = ${v} is outside [${low}, ${high}]`);

    // Always with a decimal point, which is the shape APVTS itself writes a value in.
    return `  <PARAM id="${k}" value="${Number.isInteger (v) ? v.toFixed (1) : v}"/>`;
  });

  return `<MiXXynthState>\n${lines.join ('\n')}\n</MiXXynthState>\n`;
}

const dir = join (process.cwd (), 'presets');
mkdirSync (dir, { recursive: true });

const seen = new Set ();

for (const spec of PATCHES) {
  const file = `${spec.cat} - ${spec.name}.xml`;

  if (seen.has (file))
    throw new Error (`two patches named ${file}`);

  seen.add (file);

  writeFileSync (join (dir, file), toXml (spec), 'utf8');
}

console.log (`wrote ${PATCHES.length} presets to ${dir}`);
