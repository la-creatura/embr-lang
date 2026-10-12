let total = 0, i = 0;
while (i < 3000000) {
  total = total + i % 7;
  i = i + 1;
}
console.log(String(total));
