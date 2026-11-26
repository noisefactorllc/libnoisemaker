search synth, filter
noise().write(o1, _id: "save")
read(_id: "again", surface: o1).invert().write(o0, _id: "out")
render(o0)
