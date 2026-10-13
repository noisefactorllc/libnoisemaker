#pragma once

// JavaScript's Math functions where C++'s differ.
//
// Math.round rounds halves toward +Infinity and keeps -0. Math.max and
// Math.min of two numbers return NaN when either is NaN and order +0 above
// -0. Math.pow gives NaN for a NaN exponent and for 1 or -1 to an infinite
// power, where C's pow gives 1; it equals V8's when the result is exact.
//
// Math.sin, Math.cos and Math.log are correctly rounded, as Chromium's V8
// returns them; the platform libm need not be (Apple's differs in about 3%
// of sin and cos results and 0.07% of log results at 1 ulp). They evaluate in
// double-double arithmetic (about 104 bits) and round once. In
// noisemaker-for-qt they reproduced Chromium 153's V8 on 1.8 million random
// arguments. Node's V8 is not correctly rounded: at Node 26.10 it differs
// from these in the last bit of 60,146 of 2,000,000 sin results and 65,977
// cos results. Arguments below 2^20 reduce with pi / 2 in four parts,
// larger ones with Payne and Hanek's reduction against 1280 bits of 2 / pi,
// so no result depends on the platform's libm.

namespace libnoisemaker::js {

double round(double x);
double max(double a, double b);
double min(double a, double b);
double pow(double base, double exponent);
double sin(double x);
double cos(double x);
double log(double x);

}  // namespace libnoisemaker::js
