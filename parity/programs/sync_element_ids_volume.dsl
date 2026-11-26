search synth3d, render
noise3d(_id: "vol").write3d(vol0, geo0, _id: "save")
read3d(_id: "again", tex3d: vol0, geo: geo0).render3d().write(o0)
render(o0)
