// Decompress the web game's models for the Kaigen cook: meshopt -> plain buffers, quantized -> float, WebP -> PNG.
// usage: node convert.mjs <in.glb> <out.glb> [--inspect]
import { NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS } from '@gltf-transform/extensions';
import { dequantize, unpartition } from '@gltf-transform/functions';
import { MeshoptDecoder } from 'meshoptimizer';
import sharp from 'sharp';

const [, , input, output, flag] = process.argv;
await MeshoptDecoder.ready;
const io = new NodeIO().registerExtensions(ALL_EXTENSIONS).registerDependencies({ 'meshopt.decoder': MeshoptDecoder });
const doc = await io.read(input);
const root = doc.getRoot();

if (flag === '--inspect') {
  const depth = (n) => { let d = 0; for (let p = n.getParentNode(); p; p = p.getParentNode()) d++; return d; };
  for (const n of root.listNodes()) {
    const t = n.getTranslation().map((v) => v.toFixed(3)).join(',');
    console.log(' '.repeat(depth(n) * 2) + n.getName(), n.getMesh() ? '[mesh ' + n.getMesh().listPrimitives().length + ']' : '', n.getSkin() ? '[skin]' : '', 't=' + t,
      's=' + n.getScale().map((v) => v.toFixed(3)).join(','));
  }
  for (const a of root.listAnimations()) console.log('anim', JSON.stringify(a.getName()), a.listChannels().length, 'channels');
  process.exit(0);
}

await doc.transform(dequantize());
for (const tex of root.listTextures()) {
  if (tex.getMimeType() !== 'image/webp') continue;
  const png = await sharp(Buffer.from(tex.getImage())).png().toBuffer();
  tex.setImage(new Uint8Array(png)).setMimeType('image/png');
  if (tex.getURI()) tex.setURI(tex.getURI().replace(/\.webp$/i, '.png'));
}
for (const ext of root.listExtensionsUsed()) {
  const name = ext.extensionName;
  if (name === 'EXT_meshopt_compression' || name === 'KHR_mesh_quantization' || name === 'EXT_texture_webp') ext.dispose();
}
await doc.transform(unpartition());
await io.write(output, doc);
const used = root.listExtensionsUsed().map((e) => e.extensionName);
console.log('wrote', output, 'extensions:', used.join(',') || 'none');
