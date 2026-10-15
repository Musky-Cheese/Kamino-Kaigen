# Model converters

The source models are the web game's `public/models/*.glb` (github.com/Musky-Cheese/Kamino), which are meshopt-compressed
with WebP textures; Kaigen's importer needs plain glTF. Run `npm install` here, then:

```
node b1rig.mjs     <web>/b1.glb     ../../assets/kamino/models/droids/b1.glb      # B1: rigid limbs + E-5 + B1Muzzle
node b2rig.mjs     <web>/b2.glb     ../../assets/kamino/models/droids/b2.glb      # B2: fitted, own clips kept
node clonerig.mjs  <web>/clone.glb  ../../assets/kamino/models/clone/clone.glb    # clone: skin made rigid + DC-15A
node fit.mjs <web>/gunship.glb ../../assets/kamino/models/vehicles/gunship.glb Gunship 17.5 0 180 -13.64
node fit.mjs <web>/lander.glb  ../../assets/kamino/models/vehicles/lander.glb  Lander 22 0 -90 0
node fit.mjs <web>/vulture.glb ../../assets/kamino/models/vehicles/vulture.glb Vulture 8 0 0 0 center
```

After converting, run `hz\hzbuild cook` twice (generated materials cook on the second pass).
`convert.mjs <in> x --inspect` prints a model's node tree.
