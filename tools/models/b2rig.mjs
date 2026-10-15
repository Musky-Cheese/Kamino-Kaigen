// B2 super battle droid for Kaigen: decompressed, wrapped in a "B2" root that fits it to 2.05 m with the feet at y -1
// (the drone origin is the hips), plus the B2Muzzle wrist-blaster socket. Its own skin and clips are kept.
// usage: node b2rig.mjs <b2.glb> <out.glb>
import { NodeIO, getBounds } from '@gltf-transform/core';
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
const scene = root.getDefaultScene() || root.listScenes()[0];
const node = (name) => root.listNodes().find((n) => n.getName() === name);
const wpos = (n) => { const m = n.getWorldMatrix(); return [m[12], m[13], m[14]]; };

// rest-pose extent: each vertex skinned by its joints' world x inverse bind, as the droid stands unanimated
const skinned = root.listNodes().find((n) => n.getSkin());
const skin = skinned.getSkin();
const joints = skin.listJoints();
const ibm = skin.getInverseBindMatrices();
const mul = (a, b) => { const o = new Array(16).fill(0); for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) for (let k = 0; k < 4; k++) o[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k]; return o; };
const skinMats = joints.map((j, i) => mul(j.getWorldMatrix(), ibm.getElement(i, [])));
let lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
for (const prim of skinned.getMesh().listPrimitives()) {
  const pos = prim.getAttribute('POSITION'), J = prim.getAttribute('JOINTS_0'), W = prim.getAttribute('WEIGHTS_0');
  for (let i = 0; i < pos.getCount(); i++) {
    const v = pos.getElement(i, []), ji = J.getElement(i, []), wi = W.getElement(i, []);
    const p = [0, 0, 0];
    for (let k = 0; k < 4; k++) { if (!wi[k]) continue; const m = skinMats[ji[k]]; for (let r = 0; r < 3; r++) p[r] += wi[k] * (m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r]); }
    for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); }
  }
}
const s = 2.05 / (hi[1] - lo[1]);
const cx = (lo[0] + hi[0]) / 2, cz = (lo[2] + hi[2]) / 2;
console.log('bind bbox', lo.map((v) => v.toFixed(2)).join(','), hi.map((v) => v.toFixed(2)).join(','), 'scale', s.toFixed(5));

const top = doc.createNode('B2').setScale([s, s, s]).setTranslation([-cx * s, -lo[1] * s - 1.0, -cz * s]);
for (const child of scene.listChildren()) { scene.removeChild(child); top.addChild(child); }
scene.addChild(top);

// the wrist blaster: just ahead of the right palm (the droid's right is -X), model space
const palm = wpos(node('palm.r_026'));
const toModel = (p) => [(p[0] - cx) * s, (p[1] - lo[1]) * s - 1.0, (p[2] - cz) * s];
const muzzle = toModel(palm);
muzzle[1] += 0.04;
muzzle[2] += 0.14;
top.addChild(doc.createNode('B2Muzzle').setTranslation(muzzle.map((v) => +v.toFixed(4))));
console.log('muzzle', muzzle.map((v) => v.toFixed(3)).join(','), 'palm', toModel(palm).map((v) => v.toFixed(3)).join(','));

for (const m of root.listMaterials()) m.setName('B2_' + m.getName());
for (const a of root.listAnimations()) a.setName(a.getName().trim().replace(/[^A-Za-z0-9_.]+/g, '_'));
for (const tex of root.listTextures()) {
  if (tex.getMimeType() !== 'image/webp') continue;
  const png = await sharp(Buffer.from(tex.getImage())).png().toBuffer();
  tex.setImage(new Uint8Array(png)).setMimeType('image/png');
}
for (const ext of root.listExtensionsUsed())
  if (['EXT_meshopt_compression', 'KHR_mesh_quantization', 'EXT_texture_webp'].includes(ext.extensionName)) ext.dispose();
await doc.transform(prune({ keepLeaves: true }), unpartition());
await io.write(output, doc);
console.log('clips', root.listAnimations().map((a) => a.getName()).join(' | '));
console.log('materials', root.listMaterials().map((m) => m.getName()).join(' | '));
