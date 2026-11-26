search synth, filter
let n = noise(_id: "pond", octaves: 3)
n().write(o0)
n().invert().write(o1)
render(o1)
