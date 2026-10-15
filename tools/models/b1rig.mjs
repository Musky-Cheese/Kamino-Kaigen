// B1 battle droid for Kaigen: rigid parts flattened to model space (metres, faces +Z, feet at y -1) and grouped into
// named limbs. Each limb node sits at its joint pivot; extras carry the debris radius and the child joint ("tip").
// Adds an E-5 blaster and the B1Muzzle socket. usage: node b1rig.mjs <b1.glb> <out.glb>
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

const mul = (a, b) => { const o = new Array(16).fill(0); for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) for (let k = 0; k < 4; k++) o[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k]; return o; };
const xf = (m, p) => [m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13], m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]];
const xd = (m, d) => { const v = [m[0] * d[0] + m[4] * d[1] + m[8] * d[2], m[1] * d[0] + m[5] * d[1] + m[9] * d[2], m[2] * d[0] + m[6] * d[1] + m[10] * d[2]]; const l = Math.hypot(...v) || 1; return v.map((x) => x / l); };
const node = (name) => root.listNodes().find((n) => n.getName() === name);
const wpos = (name) => { const m = node(name).getWorldMatrix(); return [m[12], m[13], m[14]]; };

// the fit: uniform scale to 1.92 m, centred in x/z, feet at y -1
let mn = [1e9, 1e9, 1e9], mx = [-1e9, -1e9, -1e9];
const parts = root.listNodes().filter((n) => n.getMesh());
for (const n of parts) {
  const m = n.getWorldMatrix();
  for (const prim of n.getMesh().listPrimitives()) {
    const pos = prim.getAttribute('POSITION');
    for (let i = 0; i < pos.getCount(); i++) {
      const p = xf(m, pos.getElement(i, []));
      for (let k = 0; k < 3; k++) { mn[k] = Math.min(mn[k], p[k]); mx[k] = Math.max(mx[k], p[k]); }
    }
  }
}
const s = 1.92 / (mx[1] - mn[1]);
const cx = (mn[0] + mx[0]) / 2, cz = (mn[2] + mx[2]) / 2;
// faces +Z when the head is ahead of the body centre; otherwise turn it round
const flip = 1; // the hands reach forward along +Z in the source
const fit = [s * flip, 0, 0, 0, 0, s, 0, 0, 0, 0, s * flip, 0, -cx * s * flip, -mn[1] * s - 1.0, -cz * s * flip, 1];
const F = (p) => xf(fit, p);
console.log('scale', s.toFixed(4), 'flip', flip, 'bbox', mn.map((v) => v.toFixed(2)).join(','), mx.map((v) => v.toFixed(2)).join(','));

// classify the source part (the mesh node's parent name) into a limb; side by model-space x (+x is the droid's left)
const limbOf = (srcName, x) => {
  const side = x > 0 ? 'L' : 'R';
  if (/^Cabeza/.test(srcName)) return 'Head';
  if (/^Cuello/.test(srcName)) return 'Neck';
  if (/^Pecho/.test(srcName)) return 'Chest';
  if (/^Abdomen/.test(srcName)) return 'Pelvis';
  if (/^Pierna-1/.test(srcName)) return 'LegUpper' + side;
  if (/^Pierna-2/.test(srcName)) return 'LegLower' + side;
  if (/^Pie/.test(srcName)) return 'Foot' + side;
  if (/^Brazo-1/.test(srcName)) return 'ArmUpper' + side;
  if (/^Brazo-2/.test(srcName)) return 'ArmLower' + side;
  if (/^(Mano|Dedos|Pulgar)/.test(srcName)) return 'Hand' + side;
  throw new Error('unknown part ' + srcName);
};

// joints in model space; bone names' L/R do not follow the side, so each is placed by its x
const J = {};
const side = (p) => (p[0] > 0 ? 'L' : 'R');
for (const b of ['Bone.004.L_9', 'Bone.004.R_14']) { const p = F(wpos(b)); J['hip' + side(p)] = p; }
for (const b of ['Bone.005.R_7', 'Bone.005.L_12']) { const p = F(wpos(b)); J['knee' + side(p)] = p; }
for (const b of ['Bone.001.L_19', 'Bone.001.R_23']) { const p = F(wpos(b)); J['shoulder' + side(p)] = p; }
for (const b of ['Bone.002.R_17', 'Bone.002.L_21']) { const p = F(wpos(b)); J['elbow' + side(p)] = p; }
for (const b of ['Bone.009.L_37', 'Bone.009.R_48']) { const p = F(wpos(b)); J['wrist' + side(p)] = p; }
J.waist = F(wpos('Bone_5'));
J.neck = F(wpos('Bone.008_1'));

// group the source primitives by limb, baking each into model space
const groups = new Map();
for (const n of parts) {
  const src = n.getParentNode().getName();
  const m = mul(fit, n.getWorldMatrix());
  const centre = xf(m, [0, 0, 0]);
  const limb = limbOf(src, centre[0]);
  if (!groups.has(limb)) groups.set(limb, []);
  for (const prim of n.getMesh().listPrimitives()) groups.get(limb).push({ prim, m });
}
// ankles: the bottom-centre of each lower leg
const bounds = (list) => {
  const lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
  for (const { prim, m } of list) {
    const pos = prim.getAttribute('POSITION');
    for (let i = 0; i < pos.getCount(); i++) { const p = xf(m, pos.getElement(i, [])); for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); } }
  }
  return { lo, hi };
};
for (const sd of ['L', 'R']) { const b = bounds(groups.get('LegLower' + sd)); J['ankle' + sd] = [(b.lo[0] + b.hi[0]) / 2, b.lo[1] + 0.04, (b.lo[2] + b.hi[2]) / 2]; }
const rig = {
  Pelvis: [J.waist, J.waist], Chest: [J.waist, J.neck], Neck: [J.neck, J.neck], Head: [J.neck, J.neck],
};
for (const sd of ['L', 'R']) {
  rig['LegUpper' + sd] = [J['hip' + sd], J['knee' + sd]];
  rig['LegLower' + sd] = [J['knee' + sd], J['ankle' + sd]];
  rig['Foot' + sd] = [J['ankle' + sd], J['ankle' + sd]];
  rig['ArmUpper' + sd] = [J['shoulder' + sd], J['elbow' + sd]];
  rig['ArmLower' + sd] = [J['elbow' + sd], J['wrist' + sd]];
  rig['Hand' + sd] = [J['wrist' + sd], J['wrist' + sd]];
}

const buffer = root.listBuffers()[0];
const scene = doc.createScene('B1');
const top = doc.createNode('B1');
scene.addChild(top);
const bake = (prim, m, pivot) => {
  const out = prim.clone();
  for (const sem of ['POSITION', 'NORMAL', 'TANGENT']) {
    const a = prim.getAttribute(sem);
    if (!a) continue;
    const n = a.getCount(), size = a.getElementSize();
    const arr = new Float32Array(n * size);
    for (let i = 0; i < n; i++) {
      const v = a.getElement(i, []);
      const r = sem === 'POSITION' ? xf(m, v).map((x, k) => x - pivot[k]) : xd(m, v);
      for (let k = 0; k < size; k++) arr[i * size + k] = k < 3 ? r[k] : v[k];
    }
    out.setAttribute(sem, doc.createAccessor().setType(a.getType()).setArray(arr).setBuffer(buffer));
  }
  return out;
};
// one primitive per limb (they share the body material): positions, normals, uvs, indices
const merge = (prims) => {
  if (prims.length === 1) return prims[0];
  const out = prims[0].clone();
  for (const sem of ['POSITION', 'NORMAL', 'TEXCOORD_0']) {
    const parts = prims.map((p) => p.getAttribute(sem));
    if (parts.some((a) => !a)) { out.setAttribute(sem, null); continue; }
    const size = parts[0].getElementSize();
    const arr = new Float32Array(parts.reduce((n, a) => n + a.getCount() * size, 0));
    let o = 0;
    for (const a of parts) for (let i = 0; i < a.getCount(); i++) { const v = a.getElement(i, []); for (let k = 0; k < size; k++) arr[o++] = v[k]; }
    out.setAttribute(sem, doc.createAccessor().setType(parts[0].getType()).setArray(arr).setBuffer(buffer));
  }
  out.setAttribute('TANGENT', null);
  const idx = [];
  let base = 0;
  for (const p of prims) {
    const ind = p.getIndices();
    const n = p.getAttribute('POSITION').getCount();
    if (ind) for (let i = 0; i < ind.getCount(); i++) idx.push(ind.getScalar(i) + base); else for (let i = 0; i < n; i++) idx.push(i + base);
    base += n;
  }
  out.setIndices(doc.createAccessor().setType('SCALAR').setArray(base > 65535 ? new Uint32Array(idx) : new Uint16Array(idx)).setBuffer(buffer));
  return out;
};
const r3 = (v) => v.map((x) => +x.toFixed(4));
for (const [limb, list] of groups) {
  const [pivot, tip] = rig[limb];
  const mesh = doc.createMesh('B1' + limb);
  mesh.addPrimitive(merge(list.map(({ prim, m }) => bake(prim, m, pivot))));
  const b = bounds(list);
  const radius = Math.max(0.05, Math.hypot(b.hi[0] - b.lo[0], b.hi[1] - b.lo[1], b.hi[2] - b.lo[2]) * 0.35);
  const nd = doc.createNode('B1' + limb).setTranslation(r3(pivot)).setMesh(mesh)
    .setExtras({ radius: +radius.toFixed(3), tip_x: +tip[0].toFixed(4), tip_y: +tip[1].toFixed(4), tip_z: +tip[2].toFixed(4) });
  top.addChild(nd);
}

// the E-5 blaster: boxes along +Z, held at the right hand's height in front of the chest; it rides the chest
const gunMat = doc.createMaterial('Gun_E5').setBaseColorFactor([0.09, 0.09, 0.1, 1]).setMetallicFactor(0.6).setRoughnessFactor(0.45);
const box = (c, h) => {
  const P = [], N = [], I = [];
  const faces = [[0, 1], [0, -1], [1, 1], [1, -1], [2, 1], [2, -1]];
  for (const [ax, sg] of faces) {
    const u = (ax + 1) % 3, v = (ax + 2) % 3, base = P.length / 3;
    for (const [a, b] of [[-1, -1], [1, -1], [1, 1], [-1, 1]]) {
      const p = [0, 0, 0]; p[ax] = c[ax] + sg * h[ax]; p[u] = c[u] + a * h[u]; p[v] = c[v] + b * h[v];
      P.push(...p); const n = [0, 0, 0]; n[ax] = sg; N.push(...n);
    }
    if (sg > 0) I.push(base, base + 1, base + 2, base, base + 2, base + 3); else I.push(base, base + 2, base + 1, base, base + 3, base + 2);
  }
  return { P, N, I };
};
const grip = [-0.04, 0.22, 0.08];
const gunParts = [
  box([0, 0, 0.22], [0.03, 0.045, 0.32]),   // body
  box([0, 0.01, 0.62], [0.016, 0.016, 0.1]), // barrel
  box([0, -0.07, 0.02], [0.018, 0.05, 0.03]),// grip
  box([0, 0.065, 0.2], [0.012, 0.02, 0.1]),  // sight
  box([0, -0.02, -0.12], [0.02, 0.035, 0.06]),// stock
];
const P = [], N = [], I = [];
for (const g of gunParts) { const o = P.length / 3; P.push(...g.P); N.push(...g.N); I.push(...g.I.map((i) => i + o)); }
const gunPrim = doc.createPrimitive().setMaterial(gunMat)
  .setAttribute('POSITION', doc.createAccessor().setType('VEC3').setArray(new Float32Array(P)).setBuffer(buffer))
  .setAttribute('NORMAL', doc.createAccessor().setType('VEC3').setArray(new Float32Array(N)).setBuffer(buffer))
  .setIndices(doc.createAccessor().setType('SCALAR').setArray(new Uint16Array(I)).setBuffer(buffer));
top.addChild(doc.createNode('B1Gun').setTranslation(r3(grip)).setMesh(doc.createMesh('B1Gun').addPrimitive(gunPrim))
  .setExtras({ radius: 0.25, tip_x: +(grip[0] + 0.02).toFixed(4), tip_y: +(grip[1] + 0.01).toFixed(4), tip_z: +(grip[2] + 0.14).toFixed(4) }));
top.addChild(doc.createNode('B1Muzzle').setTranslation(r3([grip[0], grip[1] + 0.01, grip[2] + 0.73])));

for (const sc of root.listScenes()) if (sc !== scene) sc.dispose();
root.setDefaultScene(scene);
for (const m of root.listMaterials()) if (m !== gunMat) m.setName('B1_Body');
for (const tex of root.listTextures()) {
  if (tex.getMimeType() !== 'image/webp') continue;
  const png = await sharp(Buffer.from(tex.getImage())).resize(1024, 1024).png().toBuffer();
  tex.setImage(new Uint8Array(png)).setMimeType('image/png');
}
for (const ext of root.listExtensionsUsed())
  if (['EXT_meshopt_compression', 'KHR_mesh_quantization', 'EXT_texture_webp'].includes(ext.extensionName)) ext.dispose();
await doc.transform(prune({ keepLeaves: true }), unpartition());
await io.write(output, doc);
for (const [k, v] of Object.entries(J)) console.log(k.padEnd(10), r3(v).join(', '));
console.log('limbs', [...groups.keys()].join(' '));
