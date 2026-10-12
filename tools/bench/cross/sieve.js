const n = 2000000;
const flags = new Array(n + 1).fill(1);
flags[0] = 0; flags[1] = 0;
let p = 2;
while (p * p <= n) {
  if (flags[p] === 1) {
    let m = p * p;
    while (m <= n) { flags[m] = 0; m = m + p; }
  }
  p = p + 1;
}
let count = 0, i = 0;
while (i <= n) { count = count + flags[i]; i = i + 1; }
console.log(String(count));
