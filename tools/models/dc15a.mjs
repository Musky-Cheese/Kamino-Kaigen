// DC-15A blaster rifle for Kaigen: boxes under the template rifle's node contract (Body, Magazine, MagCell, ChargingHandle,
// AmmoScreen, Vents meshes; Muzzle / EjectionPort / Sight sockets; -Z forward, grips where the arm clips hold them).
// usage: node dc15a.mjs <out.glb>
import { Document, NodeIO } from '@gltf-transform/core';

const [, , output] = process.argv;
const doc = new Document();
const buffer = doc.createBuffer();
const scene = doc.createScene('Scene');
const r4 = (v) => Math.round(v * 1e4) / 1e4;
const acc = (type, arr) => doc.createAccessor().setType(type).setArray(arr).setBuffer(buffer);

const white = doc.createMaterial('rifle_Atlas').setBaseColorFactor([0.86, 0.88, 0.9, 1]).setMetallicFactor(0.15).setRoughnessFactor(0.45);
const dark = doc.createMaterial('DC_Dark').setBaseColorFactor([0.07, 0.075, 0.085, 1]).setMetallicFactor(0.55).setRoughnessFactor(0.5);
const vents = doc.createMaterial('Emit_Vents').setBaseColorFactor([0.1, 0.1, 0.1, 1]).setEmissiveFactor([0.3, 0.6, 1]).setRoughnessFactor(0.6);
const cell = doc.createMaterial('Emit_Cell').setBaseColorFactor([0.1, 0.1, 0.1, 1]).setEmissiveFactor([0.3, 0.7, 1]).setRoughnessFactor(0.6);
const status = doc.createMaterial('Emit_Status').setBaseColorFactor([0.1, 0.1, 0.1, 1]).setEmissiveFactor([0.2, 1, 0.5]).setRoughnessFactor(0.6);
const screen = doc.createMaterial('Screen').setBaseColorFactor([0.05, 0.05, 0.06, 1]).setRoughnessFactor(0.3);

// an axis-aligned box at centre c with half extents h, planar UVs per face
const box = (c, h, P, N, U, I) => {
  for (const [ax, sg] of [[0, 1], [0, -1], [1, 1], [1, -1], [2, 1], [2, -1]]) {
    const u = (ax + 1) % 3, v = (ax + 2) % 3, base = P.length / 3;
    for (const [a, b] of [[-1, -1], [1, -1], [1, 1], [-1, 1]]) {
      const p = [0, 0, 0]; p[ax] = c[ax] + sg * h[ax]; p[u] = c[u] + a * h[u]; p[v] = c[v] + b * h[v];
      P.push(...p); const nn = [0, 0, 0]; nn[ax] = sg; N.push(...nn); U.push((a + 1) * 0.5, (b + 1) * 0.5);
    }
    if (sg > 0) I.push(base, base + 1, base + 2, base, base + 2, base + 3); else I.push(base, base + 2, base + 1, base, base + 3, base + 2);
  }
};
const prim = (mat, P, N, U, I) => doc.createPrimitive().setMaterial(mat)
  .setAttribute('POSITION', acc('VEC3', new Float32Array(P))).setAttribute('NORMAL', acc('VEC3', new Float32Array(N)))
  .setAttribute('TEXCOORD_0', acc('VEC2', new Float32Array(U))).setIndices(acc('SCALAR', new Uint16Array(I)));
// a mesh of boxes per material: [[material, [[centre, half], ...]], ...]
const meshOf = (name, groups) => {
  const mesh = doc.createMesh(name);
  for (const [mat, boxes] of groups) {
    const P = [], N = [], U = [], I = [];
    for (const [c, h] of boxes) box(c, h, P, N, U, I);
    mesh.addPrimitive(prim(mat, P, N, U, I));
  }
  return mesh;
};
const root = doc.createNode('Rifle');
scene.addChild(root);
const node = (name, t, mesh) => { const n = doc.createNode(name).setTranslation(t.map(r4)); if (mesh) n.setMesh(mesh); root.addChild(n); return n; };

// the body: receiver, top rail, open stock, barrel shroud, long barrel and its flared muzzle, white; grips, scope and guard dark
node('Body', [0, 0, 0], meshOf('Body', [
  [white, [
    [[0, 0.03, -0.05], [0.034, 0.046, 0.22]],       // receiver
    [[0, 0.084, -0.08], [0.02, 0.01, 0.2]],          // top cover
    [[0, 0.01, 0.3], [0.018, 0.03, 0.14]],           // stock spine
    [[0, -0.012, 0.44], [0.024, 0.055, 0.012]],      // butt plate
    [[0, -0.05, 0.34], [0.012, 0.02, 0.09]],         // stock lower bar
    [[0, 0.036, -0.45], [0.024, 0.024, 0.19]],       // barrel shroud
    [[0, 0.036, -0.78], [0.013, 0.013, 0.17]],       // barrel
    [[0, 0.036, -0.975], [0.026, 0.026, 0.03]],      // muzzle flare
    [[0, 0.036, -1.0], [0.016, 0.016, 0.012]],       // muzzle lip
  ]],
  [dark, [
    [[0, -0.078, 0.035], [0.016, 0.045, 0.022]],     // pistol grip (GripR)
    [[0, -0.022, -0.33], [0.016, 0.03, 0.05]],       // foregrip (GripL)
    [[0, -0.045, -0.0], [0.012, 0.004, 0.035]],      // trigger guard
    [[0, 0.139, -0.08], [0.015, 0.002, 0.1]],        // scope: an open frame the aim looks through
    [[0, 0.107, -0.08], [0.015, 0.002, 0.1]],
    [[0.015, 0.123, -0.08], [0.002, 0.016, 0.1]],
    [[-0.015, 0.123, -0.08], [0.002, 0.016, 0.1]],
    [[0, 0.1, -0.03], [0.01, 0.012, 0.012]],         // scope mount
    [[0, 0.1, -0.14], [0.01, 0.012, 0.012]],
    [[0, 0.036, -0.64], [0.03, 0.03, 0.012]],        // shroud collar
  ]],
  [status, [[[0.036, 0.062, 0.0], [0.003, 0.004, 0.012]]]], // status lamp, right side
]));
node('Vents', [0, 0, 0], meshOf('Vents', [[vents, [
  [[0.0245, 0.036, -0.4], [0.002, 0.007, 0.07]],
  [[-0.0245, 0.036, -0.4], [0.002, 0.007, 0.07]],
  [[0.0245, 0.036, -0.56], [0.002, 0.007, 0.05]],
  [[-0.0245, 0.036, -0.56], [0.002, 0.007, 0.05]],
]]]));
// the power cell: the magazine the reload pulls, and its glowing cell face
node('Magazine', [0, -0.03, -0.115], meshOf('Magazine', [[dark, [[[0, -0.03, 0], [0.022, 0.04, 0.045]]]]]));
node('MagCell', [-0.0225, -0.06, -0.115], meshOf('MagCell', [[cell, [[[0, 0, 0], [0.002, 0.014, 0.03]]]]]));
node('ChargingHandle', [0.031, 0.062, 0.055], meshOf('ChargingHandle', [[dark, [[[0, 0, 0], [0.014, 0.007, 0.022]]]]]));
// the ammo screen: a quad on the receiver's left face, UV 0..1 for the canvas
{
  const x = -0.0345, y = 0.05, z = 0.03, w = 0.03, h = 0.022;
  const P = [x, y - h, z - w, x, y - h, z + w, x, y + h, z + w, x, y + h, z - w];
  const N = [-1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0];
  const U = [1, 1, 0, 1, 0, 0, 1, 0];
  const I = [0, 2, 1, 0, 3, 2];
  const mesh = doc.createMesh('AmmoScreen').addPrimitive(prim(screen, P, N, U, I));
  node('AmmoScreen', [0, 0, 0], mesh);
}
node('Muzzle', [0, 0.036, -1.01]);
node('EjectionPort', [0.036, 0.05, -0.078]);
node('Sight', [0, 0.123, -0.086]);
node('GripL', [0, -0.014, -0.33]);
node('GripR', [0, -0.084, 0.033]);

await new NodeIO().write(output, doc);
console.log('wrote', output, 'nodes', root.listChildren().map((n) => n.getName()).join(' | '));
