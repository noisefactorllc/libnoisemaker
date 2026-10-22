search synth, points, render

perlin(scale: 22, octaves: 4, colorMode: mono).write(o1)

// fieldTex is a deprecated alias of heightTex on flow, dla and physarum (S007).
solid().pointsEmit(stateSize: 64).flow(fieldTex: read(o1)).pointsRender().write(o0)
solid().pointsEmit(stateSize: 64).dla(fieldTex: read(o1)).pointsRender().write(o2)

// Old and new name together: the new name wins and the old key is dropped.
solid().pointsEmit(stateSize: 64).physarum(heightTex: read(o1), fieldTex: read(o2)).pointsRender().write(o3)

// hydraulic has no alias, so fieldTex is an unknown argument there.
solid().pointsEmit(stateSize: 64).hydraulic(fieldTex: read(o1)).pointsRender().write(o4)

render(o0)
