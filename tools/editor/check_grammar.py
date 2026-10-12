#!/usr/bin/env python3
"""smoke test for tools/editor/vscode/syntaxes/embr.tmLanguage.json (not a full TextMate engine).

  1. the JSON loads and every `match`/`begin`/`end` regex compiles (Python's `re` is close enough to
     Oniguruma for the constructs used here: no variable-width lookbehind, no \\h, no possessive);
  2. the reserved-word set in the grammar equals the list on the wiki's lexical-grammar page, so adding a keyword
     to the language without the grammar fails. needs a clone of the wiki (--wiki DIR, EMBR_WIKI_DIR, or the wiki/
     submodule), without one this part is skipped;
  3. known snippets match the rule they should (and known-bad ones hit the `invalid.*` rules).

exit status 0 = ok.
"""
import json, os, re, sys, pathlib

root = pathlib.Path(__file__).resolve().parents[2]
grammar = json.loads((root / "tools/editor/vscode/syntaxes/embr.tmLanguage.json").read_text())
repo = grammar["repository"]
errors = []

# ---- 1. every regex compiles --------------------------------------------------------------
def walk(node, where):
    if isinstance(node, dict):
        for k in ("match", "begin", "end"):
            if k in node:
                try:
                    re.compile(node[k])
                except re.error as e:
                    errors.append(f"{where}: bad {k} regex {node[k]!r}: {e}")
        for k, v in node.items():
            walk(v, f"{where}/{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            walk(v, f"{where}[{i}]")
walk(grammar, "grammar")

def patterns(rule):
    r = repo[rule]
    return r["patterns"] if "patterns" in r else [r]

def scope_of(rule, text):
    """scope names (the rule's own `name` plus any capture-group names) of every pattern in `rule`
    whose match/begin regex finds something in `text`."""
    out = []
    def visit(p):
        rx = p.get("match") or p.get("begin")
        if rx and re.search(rx, text):
            out.append(p.get("name", ""))
            out.extend(c.get("name", "") for c in p.get("captures", {}).values())
        for sub in p.get("patterns", []):
            visit(sub)
    visit(repo[rule])
    return out

# ---- 2. keyword list == the wiki's lexical-grammar page ------------------------------------
wiki_dir = os.environ.get("EMBR_WIKI_DIR", "") or (str(root / "wiki") if (root / "wiki" / "lexical-grammar.md").exists() else "")
if "--wiki" in sys.argv:
    wiki_dir = sys.argv[sys.argv.index("--wiki") + 1]
gram_kw = set()
for p in patterns("keywords"):
    mm = re.match(r"\\b\(?([a-z|]+)\)?\\b", p["match"])
    if mm: gram_kw |= set(mm.group(1).split("|"))
spec_kw = gram_kw
if not wiki_dir:
    print("no wiki clone given (--wiki DIR, EMBR_WIKI_DIR or the wiki/ submodule): skipping the keyword comparison")
else:
    page = pathlib.Path(wiki_dir) / "lexical-grammar.md"
    if not page.exists():
        print(f"{page} not found (an old copy of the wiki? git -C {wiki_dir} pull): skipping the keyword comparison")
        wiki_dir = ""
    else:
        spec = page.read_text()
        m = re.search(r"### reserved keywords[^\n]*\n\s*```\n(.*?)```", spec, re.S)
        spec_kw = set(m.group(1).split()) if m else set()
        m2 = re.search(r"### reserved keywords \((\d+)\)", spec)
        if not m2 or int(m2.group(1)) != len(spec_kw):
            errors.append(f"the wiki says {m2.group(1) if m2 else '?'} reserved keywords but lists {len(spec_kw)}: {sorted(spec_kw)}")
        if gram_kw != spec_kw:
            errors.append(f"keyword drift: only in grammar {sorted(gram_kw - spec_kw)}, only in the wiki {sorted(spec_kw - gram_kw)}")

# ---- 3. behavioural snippets ---------------------------------------------------------------
def expect(rule, text, scope_fragment, present=True):
    got = scope_of(rule, text)
    ok = any(scope_fragment in s for s in got)
    if ok != present:
        errors.append(f"{rule}: {text!r} {'should' if present else 'should NOT'} match *{scope_fragment}* (got {got})")

expect("numbers", "42", "integer")
expect("numbers", "3.14", "float")
expect("numbers", "1e3", "float")
expect("numbers", "2.5e-3", "float")
expect("numbers", "1E+10", "float")
expect("numbers", "e5", "float", present=False)            # an identifier, not an exponent
expect("numbers", "x1", "integer", present=False)       # digits inside an identifier are not numbers
expect("numbers", "1.", "invalid")                        # trailing decimal point is a lexer error
expect("numbers", "1.2.3", "invalid")                     # second decimal point too
expect("strings", r'"a\nb"', "string")
expect("fn-definition", "fn add(a, b)", "function")
expect("type-annotation", "x: int", "support.type")
expect("type-annotation", "-> num", "support.type")
expect("type-annotation", "local int x = 1", "modifier")
expect("operators", "x += 1", "compound")
expect("operators", "a..b", "invalid")
expect("operators", "a...b", "spread")
expect("operators", "a != b", "comparison")
expect("function-call", "print(1)", "call")
expect("function-call", "x = 1", "call", present=False)
expect("comments", "# hi", "line")
expect("comments", "#[ block ]#", "block")
expect("constants", "true", "constant")
esc = repo["strings"]["patterns"]
for good in (r"\x41", r"\u00e9", r"\u{1F600}"):
    if not re.search(esc[0]["match"], good): errors.append(f"string escape {good} should be valid")
for bad in (r"\x4", r"\u12", r"\u{}"):
    if re.fullmatch(esc[0]["match"], bad): errors.append(f"string escape {bad} should NOT be a complete valid escape")
if not re.search(esc[0]["match"], r"\n") or re.search(esc[0]["match"], r"\q") or not re.search(esc[1]["match"], r"\q"):
    errors.append("string escapes: \\n must be valid and \\q illegal")

# every .embr file in the repo must lex cleanly by the same rules the grammar encodes: only the valid
# escapes inside strings (scanned left to right, so `\\b` is one escaped backslash followed by `b`,
# exactly like the real lexer), no `1.`/`1.2.3` numbers outside strings, no bare `..`
VALID_ESC = set('ntr0"\\xu')
def scan_line(line):
    i, n, problems = 0, len(line), []
    while i < n:
        c = line[i]
        if c == "#":                      # comment to end of line (block comments handled by the caller)
            break
        if c == '"':
            i += 1
            while i < n and line[i] != '"':
                if line[i] == "\\":
                    if i + 1 >= n or line[i + 1] not in VALID_ESC:
                        problems.append("unknown string escape")
                    i += 2
                else:
                    i += 1
            i += 1
            continue
        i += 1
    code = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line.split("#")[0])
    for p in illegal_num:
        if re.search(p["match"], code): problems.append(p["name"])
    return problems
illegal_num = [p for p in repo["numbers"]["patterns"] if "invalid" in p.get("name", "")] + \
              [p for p in repo["operators"]["patterns"] if "invalid" in p.get("name", "")]
for f in list(root.glob("modules/*.embr")) + list(root.glob("tools/bench/*.embr")):
    in_block = False
    for n, line in enumerate(f.read_text().splitlines(), 1):
        if in_block:
            if "]#" in line: in_block = False
            continue
        if "#[" in line and "]#" not in line.split("#[", 1)[1]:
            in_block = True
            line = line.split("#[", 1)[0]
        for prob in scan_line(line):
            errors.append(f"{f.relative_to(root)}:{n}: {prob}: {line.strip()}")

if errors:
    print("GRAMMAR CHECK FAILED"); [print(" -", e) for e in errors]; sys.exit(1)
print(f"grammar ok ({len(spec_kw)} keywords" + (" in sync with the wiki)" if wiki_dir else ", wiki comparison skipped)"))
