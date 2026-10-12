const n = 200000;
const arr = [];
let i = 0;
while (i < n) { arr.push(i * 2); i = i + 1; }
let sum_ = 0, r = 0;
while (r < 5) {
  i = 0;
  while (i < n) { sum_ = sum_ + arr[i]; i = i + 1; }
  r = r + 1;
}
r = 0;
while (r < 3) {
  i = 0;
  while (i < n) { arr[i] = arr[i] + 1; i = i + 1; }
  r = r + 1;
}
i = 0;
while (i < n) { arr.push(i); i = i + 1; }
console.log(String(sum_));
console.log(String(arr[n - 1]));
console.log(String(arr.length));
