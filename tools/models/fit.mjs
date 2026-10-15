// Static model for Kaigen: decompress, then wrap the scene in a root node `name` that rotates it (degrees, XYZ),
// scales its longest horizontal extent to `length` metres and rests it on y 0, centred in x/z.
// usage: node fit.mjs <in.glb> <out.glb> <name> <length> <rx> <ry> <rz> [centerY]
import { NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS } from '@gltf-transform/extensions';
import { dequantize, prune, unpartition } from '@gltf-transform/functions';
import { MeshoptDecoder } from 'meshoptimizer';
import sharp from 'sharp';

const [, , input, output, name, lengthArg, rx, ry, rz, centerY] = process.argv;
await MeshoptDecoder.ready;
const io = new NodeIO().registerExtensions(ALL_EXTENSIONS).registerDependencies({ 'meshopt.decoder': MeshoptDecoder });
const doc = await io.read(input);
await doc.transform(dequantize());
const root = doc.getRoot();
const scene = root.getDefaultScene() || root.listScenes()[0];

const d2r = Math.PI / 180;
const quatFromEuler = (x, y, z) => {
  const [cx, sx, cy, sy, cz, sz] = [Math.cos(x / 2), Math.sin(x / 2), Math.cos(y / 2), Math.sin(y / 2), Math.cos(z / 2), Math.sin(z / 2)];
  return [sx * cy * cz + cx * sy * sz, cx * sy * cz - sx * cy * sz, cx * cy * sz + sx * sy * cz, cx * cy * cz - sx * sy * sz];
};
const q = quatFromEuler(+rx * d2r, +ry * d2r, +rz * d2r);
const rot = (v) => {
  const [x, y, z, w] = q;
  const uv = [y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]];
  const uuv = [y * uv[2] - z * uv[1], z * uv[0] - x * uv[2], x * uv[1] - y * uv[0]];
  return [v[0] + 2 * (w * uv[0] + uuv[0]), v[1] + 2 * (w * uv[1] + uuv[1]), v[2] + 2 * (w * uv[2] + uuv[2])];
};
let lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
for (const n of root.listNodes()) {
  if (!n.getMesh()) continue;
  const m = n.getWorldMatrix();
  for (const prim of n.getMesh().listPrimitives()) {
    const pos = prim.getAttribute('POSITION');
    for (let i = 0; i < pos.getCount(); i++) {
      const v = pos.getElement(i, []);
      const p = rot([0, 1, 2].map((k) => m[k] * v[0] + m[4 + k] * v[1] + m[8 + k] * v[2] + m[12 + k]));
      for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); }
    }
  }
}
const s = +lengthArg / Math.max(hi[0] - lo[0], hi[2] - lo[2]);
const yOff = centerY ? -(lo[1] + hi[1]) / 2 * s : -lo[1] * s;
// bake: every mesh node's world matrix, then the rotation, scale and offset, into its vertices; one flat root
const off = [-(lo[0] + hi[0]) / 2 * s, yOff, -(lo[2] + hi[2]) / 2 * s];
const top = doc.createNode(name);
const buffer = root.listBuffers()[0];
const baked = new Map();
for (const n of root.listNodes()) {
  const mesh = n.getMesh();
  if (!mesh) continue;
  const m = n.getWorldMatrix();
  const lin = (v) => [0, 1, 2].map((k) => m[k] * v[0] + m[4 + k] * v[1] + m[8 + k] * v[2]);
  const out = doc.createMesh(mesh.getName());
  for (const prim of mesh.listPrimitives()) {
    const p2 = prim.clone();
    for (const sem of ['POSITION', 'NORMAL', 'TANGENT']) {
      const acc = prim.getAttribute(sem);
      if (!acc) continue;
      const size = acc.getElementSize(), count = acc.getCount(), arr = new Float32Array(count * size);
      for (let i = 0; i < count; i++) {
        const v = acc.getElement(i, []);
        let r;
        if (sem === 'POSITION') { const w = lin(v).map((x, k) => x + m[12 + k]); r = rot(w).map((x, k) => x * s + off[k]); }
        else { const d = rot(lin(v)); const l = Math.hypot(...d) || 1; r = d.map((x) => x / l); }
        for (let k = 0; k < size; k++) arr[i * size + k] = k < 3 ? r[k] : v[k];
      }
      p2.setAttribute(sem, doc.createAccessor().setType(acc.getType()).setArray(arr).setBuffer(buffer));
    }
    out.addPrimitive(p2);
  }
  top.addChild(doc.createNode(n.getName() || 'part').setMesh(out));
}
for (const child of scene.listChildren()) scene.removeChild(child);
scene.addChild(top);
for (const m of root.listMaterials()) m.setName(name + '_' + (m.getName() || 'mat').replace(/[^A-Za-z0-9_]+/g, '_'));
for (const tex of root.listTextures()) {
  if (tex.getMimeType() !== 'image/webp') continue;
  const png = await sharp(Buffer.from(tex.getImage())).png().toBuffer();
  tex.setImage(new Uint8Array(png)).setMimeType('image/png');
}
for (const ext of root.listExtensionsUsed())
  if (['EXT_meshopt_compression', 'KHR_mesh_quantization', 'EXT_texture_webp'].includes(ext.extensionName)) ext.dispose();
await doc.transform(prune({ keepLeaves: true }), unpartition());
await io.write(output, doc);
const size = [0, 1, 2].map((k) => ((hi[k] - lo[k]) * s).toFixed(2));
console.log(name, 'size', size.join(' x '), 'materials', root.listMaterials().map((m) => m.getName()).join(','));
