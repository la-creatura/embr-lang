const items = [];
let i = 0;
while (i < 20000) { items.push({"id": i, "name": "item" + String(i), "tags": ["a", "b"], "score": i * 3}); i = i + 1; }
const text = JSON.stringify(items);
const back = JSON.parse(text);
console.log(String(text.length));
console.log(String(back.length));
console.log(back[12345]["name"]);
console.log(String(JSON.stringify(back).length));
