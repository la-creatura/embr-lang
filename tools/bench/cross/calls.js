function add(a, b) { return a + b; }
function twice(x) { return add(x, x); }
let acc = 0, i = 0;
while (i < 600000) {
  acc = add(acc, twice(i));
  i = i + 1;
}
console.log(String(acc));
