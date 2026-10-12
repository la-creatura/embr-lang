function makeCounter() {
  let n = 0;
  return function () { n = n + 1; return n; };
}
const c = makeCounter();
let i = 0;
while (i < 500000) { c(); i = i + 1; }
console.log(String(c()));
