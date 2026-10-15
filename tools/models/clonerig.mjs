// Clone trooper for Kaigen: the skinned Phase I trooper made rigid. Each vertex goes to the limb of its heaviest joint,
// baked in its rest pose to model space (metres, faces +Z, feet at y -1); each limb node sits at its joint pivot with the
// child joint in its extras, like the B1. Adds the DC-15A and the CTMuzzle socket. usage: node clonerig.mjs <clone.glb> <out.glb>
import { NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS } from '@gltf-transform/extensions';
import { dequantize, prune, unpartition } from '@gltf-transform/functions';
import { MeshoptDecoder } from 'meshoptimizer';
import sharp from 'sharp';

const [, , input, output] = process.argv;
await MeshoptDecoder.ready;
const io = new NodeIO().registerExtensions(ALL_EXTENSIONS).registerDependencies({ 'meshopt.decoder': MeshoptDecoder });
const doc = await io.read(input);
await doc.transform(dequantize());
const root = doc.getRoot();
const buffer = root.listBuffers()[0];
const mul = (a, b) => { const o = new Array(16).fill(0); for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) for (let k = 0; k < 4; k++) o[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k]; return o; };
const xf = (m, p) => [0, 1, 2].map((r) => m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r]);
const xd = (m, d) => [0, 1, 2].map((r) => m[r] * d[0] + m[4 + r] * d[1] + m[8 + r] * d[2]);
const node = (name) => root.listNodes().find((n) => n.getName() === name);
const wpos = (name) => { const m = node(name).getWorldMatrix(); return [m[12], m[13], m[14]]; };

const limbOfJoint = (name) => {
  const side = /^Left/.test(name) ? 'L' : /^Right/.test(name) ? 'R' : '';
  const base = name.replace(/^(Left|Right)/, '').replace(/_\d+$/, '');
  if (/^Hips/.test(base)) return 'Pelvis';
  if (/^(Spine|Shoulder)/.test(base)) return 'Chest';
  if (/^Neck/.test(base)) return 'Neck';
  if (/^Head/.test(base)) return 'Head';
  if (/^UpLeg/.test(base)) return 'LegUpper' + side;
  if (/^Leg/.test(base)) return 'LegLower' + side;
  if (/^(Foot|ToeBase)/.test(base)) return 'Foot' + side;
  if (/^Arm/.test(base)) return 'ArmUpper' + side;
  if (/^ForeArm/.test(base)) return 'ArmLower' + side;
  if (/^Hand/.test(base)) return 'Hand' + side;
  return 'Chest';
};

// every skinned vertex in its rest pose (joint world x inverse bind), with its heaviest joint's limb
const parts = [];
let lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
for (const n of root.listNodes()) {
  const skin = n.getSkin();
  if (!skin || !n.getMesh()) continue;
  const joints = skin.listJoints(), ibm = skin.getInverseBindMatrices();
  const mats = joints.map((j, i) => mul(j.getWorldMatrix(), ibm.getElement(i, [])));
  for (const prim of n.getMesh().listPrimitives()) {
    const P = prim.getAttribute('POSITION'), N = prim.getAttribute('NORMAL'), UV = prim.getAttribute('TEXCOORD_0');
    const J = prim.getAttribute('JOINTS_0'), W = prim.getAttribute('WEIGHTS_0');
    const count = P.getCount(), pos = [], nrm = [], uv = [], limb = [];
    for (let i = 0; i < count; i++) {
      const v = P.getElement(i, []), nv = N ? N.getElement(i, []) : [0, 1, 0], ji = J.getElement(i, []), wi = W.getElement(i, []);
      const p = [0, 0, 0], d = [0, 0, 0];
      let best = 0;
      for (let k = 0; k < 4; k++) {
        if (!wi[k]) continue;
        const m = mats[ji[k]], a = xf(m, v), b = xd(m, nv);
        for (let r = 0; r < 3; r++) { p[r] += wi[k] * a[r]; d[r] += wi[k] * b[r]; }
        if (wi[k] > wi[best]) best = k;
      }
      pos.push(p); nrm.push(d); uv.push(UV ? UV.getElement(i, []) : [0, 0]);
      limb.push(limbOfJoint(joints[ji[best]].getName()));
      for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); }
    }
    const ind = prim.getIndices();
    const tris = ind ? Array.from({ length: ind.getCount() }, (_, i) => ind.getScalar(i)) : Array.from({ length: count }, (_, i) => i);
    parts.push({ material: prim.getMaterial(), pos, nrm, uv, limb, tris });
  }
}

// the fit: 1.83 m tall, centred in x/z, feet at y -1; turned round when the toes point -Z
const s = 1.83 / (hi[1] - lo[1]);
const cx = (lo[0] + hi[0]) / 2, cz = (lo[2] + hi[2]) / 2;
const toe = wpos('LeftToeBase_048'), heel = wpos('LeftFoot_047');
const flip = toe[2] - heel[2] < 0 ? -1 : 1;
const F = (p) => [(p[0] - cx) * s * flip, (p[1] - lo[1]) * s - 1.0, (p[2] - cz) * s * flip];
const Fd = (d) => { const v = [d[0] * flip, d[1], d[2] * flip]; const l = Math.hypot(...v) || 1; return v.map((x) => x / l); };
console.log('scale', s.toFixed(5), 'flip', flip, 'height', ((hi[1] - lo[1]) * s).toFixed(2));

const J = {};
const side = (p) => (p[0] > 0 ? 'L' : 'R');
for (const [key, names] of Object.entries({ hip: ['LeftUpLeg_045', 'RightUpLeg_049'], knee: ['LeftLeg_046', 'RightLeg_050'],
  ankle: ['LeftFoot_047', 'RightFoot_051'], shoulder: ['LeftArm_08', 'RightArm_027'], elbow: ['LeftForeArm_09', 'RightForeArm_028'],
  wrist: ['LeftHand_010', 'RightHand_029'] }))
  for (const n of names) { const p = F(wpos(n)); J[key + side(p)] = p; }
J.waist = F(wpos('Spine_02'));
J.neck = F(wpos('Neck_05'));
J.hips = F(wpos('Hips_01'));
const rig = { Pelvis: [J.hips, J.hips], Chest: [J.waist, J.neck], Neck: [J.neck, J.neck], Head: [J.neck, J.neck] };
for (const sd of ['L', 'R']) {
  rig['LegUpper' + sd] = [J['hip' + sd], J['knee' + sd]];
  rig['LegLower' + sd] = [J['knee' + sd], J['ankle' + sd]];
  rig['Foot' + sd] = [J['ankle' + sd], J['ankle' + sd]];
  rig['ArmUpper' + sd] = [J['shoulder' + sd], J['elbow' + sd]];
  rig['ArmLower' + sd] = [J['elbow' + sd], J['wrist' + sd]];
  rig['Hand' + sd] = [J['wrist' + sd], J['wrist' + sd]];
}

// per limb, one primitive per material: triangles go to the limb of their first vertex
const limbs = new Map();
for (const part of parts) {
  for (let t = 0; t < part.tris.length; t += 3) {
    const tri = part.tris.slice(t, t + 3);
    const limb = part.limb[tri[0]];
    if (!limbs.has(limb)) limbs.set(limb, new Map());
    const byMat = limbs.get(limb);
    if (!byMat.has(part.material)) byMat.set(part.material, { pos: [], nrm: [], uv: [], idx: [], remap: new Map() });
    const g = byMat.get(part.material);
    for (const v of tri) {
      const key = part.pos === undefined ? v : `${parts.indexOf(part)}:${v}`;
      if (!g.remap.has(key)) {
        g.remap.set(key, g.pos.length / 3);
        const p = F(part.pos[v]), pivot = rig[limb][0];
        g.pos.push(p[0] - pivot[0], p[1] - pivot[1], p[2] - pivot[2]);
        g.nrm.push(...Fd(part.nrm[v]));
        g.uv.push(...part.uv[v]);
      }
      g.idx.push(g.remap.get(key));
    }
  }
}
const scene = doc.createScene('CT');
const top = doc.createNode('CT');
scene.addChild(top);
const acc = (type, arr) => doc.createAccessor().setType(type).setArray(arr).setBuffer(buffer);
const r4 = (v) => +v.toFixed(4);
for (const [limb, byMat] of limbs) {
  const [pivot, tip] = rig[limb];
  const mesh = doc.createMesh('CT' + limb);
  let n = 0;
  for (const [mat, g] of byMat) {
    n += g.pos.length / 3;
    mesh.addPrimitive(doc.createPrimitive().setMaterial(mat).setAttribute('POSITION', acc('VEC3', new Float32Array(g.pos)))
      .setAttribute('NORMAL', acc('VEC3', new Float32Array(g.nrm))).setAttribute('TEXCOORD_0', acc('VEC2', new Float32Array(g.uv)))
      .setIndices(acc('SCALAR', g.pos.length / 3 > 65535 ? new Uint32Array(g.idx) : new Uint16Array(g.idx))));
  }
  top.addChild(doc.createNode('CT' + limb).setTranslation(pivot.map(r4)).setMesh(mesh)
    .setExtras({ radius: 0.15, tip_x: r4(tip[0]), tip_y: r4(tip[1]), tip_z: r4(tip[2]) }));
  console.log(limb.padEnd(10), 'verts', n, 'pivot', pivot.map((v) => v.toFixed(3)).join(','));
}

// the DC-15A: white body, dark grip and scope, long barrel; the left hand's target is its tip extras
const white = doc.createMaterial('CT_DC15').setBaseColorFactor([0.78, 0.8, 0.8, 1]).setMetallicFactor(0.2).setRoughnessFactor(0.5);
const dark = doc.createMaterial('CT_DC15_Dark').setBaseColorFactor([0.06, 0.06, 0.07, 1]).setMetallicFactor(0.5).setRoughnessFactor(0.45);
const box = (c, h, P, N, I) => {
  for (const [ax, sg] of [[0, 1], [0, -1], [1, 1], [1, -1], [2, 1], [2, -1]]) {
    const u = (ax + 1) % 3, v = (ax + 2) % 3, base = P.length / 3;
    for (const [a, b] of [[-1, -1], [1, -1], [1, 1], [-1, 1]]) {
      const p = [0, 0, 0]; p[ax] = c[ax] + sg * h[ax]; p[u] = c[u] + a * h[u]; p[v] = c[v] + b * h[v];
      P.push(...p); const nn = [0, 0, 0]; nn[ax] = sg; N.push(...nn);
    }
    if (sg > 0) I.push(base, base + 1, base + 2, base, base + 2, base + 3); else I.push(base, base + 2, base + 1, base, base + 3, base + 2);
  }
};
const grip = [-0.1, 0.27, 0.12];
const foregrip = [grip[0] + 0.03, grip[1] + 0.03, grip[2] + 0.2];
const gun = doc.createMesh('CTGun');
for (const [mat, boxes] of [[white, [[[0, 0.03, 0.18], [0.032, 0.05, 0.36]], [[0, 0.03, -0.24], [0.03, 0.06, 0.1]]]],
  [dark, [[[0, -0.06, 0.02], [0.02, 0.06, 0.03]], [[0, 0.1, 0.12], [0.018, 0.022, 0.12]], [[0, 0.035, 0.66], [0.018, 0.018, 0.14]], [[0, -0.02, 0.32], [0.022, 0.03, 0.06]]]]]) {
  const P = [], N = [], I = [];
  for (const [c, h] of boxes) box(c, h, P, N, I);
  gun.addPrimitive(doc.createPrimitive().setMaterial(mat).setAttribute('POSITION', acc('VEC3', new Float32Array(P)))
    .setAttribute('NORMAL', acc('VEC3', new Float32Array(N))).setIndices(acc('SCALAR', new Uint16Array(I))));
}
top.addChild(doc.createNode('CTGun').setTranslation(grip).setMesh(gun)
  .setExtras({ radius: 0.25, tip_x: r4(foregrip[0]), tip_y: r4(foregrip[1]), tip_z: r4(foregrip[2]) }));
top.addChild(doc.createNode('CTMuzzle').setTranslation([grip[0], grip[1] + 0.035, grip[2] + 0.82].map(r4)));

for (const sc of root.listScenes()) if (sc !== scene) sc.dispose();
root.setDefaultScene(scene);
for (const sk of root.listSkins()) sk.dispose();
for (const a of root.listAnimations()) a.dispose();
for (const m of root.listMaterials()) if (!m.getName().startsWith('CT_')) m.setName('CT_' + m.getName());
for (const tex of root.listTextures()) {
  if (tex.getMimeType() !== 'image/webp') continue;
  const png = await sharp(Buffer.from(tex.getImage())).png().toBuffer();
  tex.setImage(new Uint8Array(png)).setMimeType('image/png');
}
for (const ext of root.listExtensionsUsed())
  if (['EXT_meshopt_compression', 'KHR_mesh_quantization', 'EXT_texture_webp'].includes(ext.extensionName)) ext.dispose();
await doc.transform(prune({ keepLeaves: true }), unpartition());
await io.write(output, doc);
console.log('materials', root.listMaterials().map((m) => m.getName()).join(' | '));
