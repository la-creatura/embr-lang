const words = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa", "lambda", "mu", "nu", "xi", "omicron", "pi", "rho", "sigma", "tau", "upsilon"];
// the product no longer fits a double, so the 31-bit multiply is done in two halves
function lcg(seed) {
  const a = 1103515245;
  return ((seed * (a & 0xffff)) + (((seed * (a >>> 16)) % 32768) * 65536) + 12345) % 2147483648;
}
let seed = 12345;
const parts = [];
let i = 0;
while (i < 200000) { seed = lcg(seed); parts.push(words[seed % 20]); i = i + 1; }
const text = parts.join(" ");
const counts = new Map();
for (const w of text.split(" ")) {
  if (counts.has(w)) counts.set(w, counts.get(w) + 1); else counts.set(w, 1);
}
console.log(String(counts.size));
console.log(String(counts.get("alpha")));
console.log(String(counts.get("upsilon")));
console.log(String(text.length));
