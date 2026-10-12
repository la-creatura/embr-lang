import json
items = []
i = 0
while i < 20000:
    items.append({"id": i, "name": "item" + str(i), "tags": ["a", "b"], "score": i * 3})
    i = i + 1
text = json.dumps(items, separators=(",", ":"))
back = json.loads(text)
print(len(text))
print(len(back))
print(back[12345]["name"])
print(len(json.dumps(back, separators=(",", ":"))))
