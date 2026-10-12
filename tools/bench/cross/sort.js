function lcg(seed) {
  const a = 1103515245;
  return ((seed * (a & 0xffff)) + (((seed * (a >>> 16)) % 32768) * 65536) + 12345) % 2147483648;
}
let seed = 42;
const a = [];
let i = 0;
while (i < 200000) { seed = lcg(seed); a.push(seed % 1000000); i = i + 1; }
const s = a.slice().sort((p, q) => p - q);
console.log(String(s[0]));
console.log(String(s[100000]));
console.log(String(s[199999]));
let total = 0;
i = 0;
while (i < 200000) { total = total + s[i]; i = i + 1000; }
console.log(String(total));
