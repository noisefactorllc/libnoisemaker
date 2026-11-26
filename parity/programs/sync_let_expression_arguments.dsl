search synth, mixer
let n = noise(octaves: 3)
let mix = blendMode(tex: n(), mix: 50)
noise().mix().write(o0)
render(o0)
