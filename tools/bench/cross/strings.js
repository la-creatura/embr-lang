let s = "", i = 0;
while (i < 100000) { s = s + "ab"; i = i + 1; }
console.log(String(s.length));
