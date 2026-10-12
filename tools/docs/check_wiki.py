#!/usr/bin/env python3
"""checks that the docs (the wiki) still tell the truth. CTest runs this (docs_examples).

  python3 tools/docs/check_wiki.py --embr bin/linux/embr --include build/linux-debug/generated

the docs live in the GitHub wiki (la-creatura/embr-lang.wiki), not in this repo, so this needs a clone of it:
pass --wiki DIR, set EMBR_WIKI_DIR, or just init the submodule (git submodule update --init), which is
found at wiki/. CMake passes it along (-DEMBR_WIKI_DIR=DIR to use another clone). without a wiki it prints a
note and exits 77, which CTest reports as skipped.

what it checks
  - ```embr-run blocks in the wiki pages are run on every backend the embr binary has (embr-run-vm: vm only).
    if the next block is ```output, stdout must match it exactly. a block with no output block just has to
    run without an error.
  - ```cpp-compile blocks are compiled (-fsyntax-only) against the generated embr.h.
  - ```cpp-check blocks are fragments of examples/hello/hello.cpp: every line must still appear there.
  - a few numbers the wiki states (keyword count, call depth, nesting depth) are compared to the source.
  - every plugin in plugins/ and every module in modules/ has a wiki page, listed in plugin-reference
    (or script-modules) and in import-name-index.
  - what each script module in modules/ says about itself (`embr module FILE --json`) matches its wiki page, the same way:
    it declares a version and description, has no problems, every public function has a doc comment and is named on the page.
  - what each plugin says about itself (`embr plugin NAME --json`) matches its wiki page: the plugin describes itself
    (version, description), every function it binds has a doc line, every function is named on the page, every
    function-looking name in the page's tables exists, and a plugin that needs a capability says so on its page.
"""
import argparse, pathlib, re, subprocess, sys, tempfile, os

root = pathlib.Path(__file__).resolve().parents[2]
failures = []

def fail(where, msg):
    failures.append(f"{where}: {msg}")

def blocks(path):
    """yields (lang, text, first_line) for every fenced block in a file."""
    return blocks_of(path.read_text())

def blocks_of(source):
    """yields (lang, text, first_line) for every fenced block in a string."""
    lines = source.split("\n")
    i = 0
    while i < len(lines):
        if lines[i].startswith("```"):
            lang, start, body = lines[i][3:].strip(), i + 1, []
            i += 1
            while i < len(lines) and not lines[i].startswith("```"):
                body.append(lines[i]); i += 1
            yield lang, "\n".join(body) + "\n", start
        i += 1

def norm(s):
    return re.sub(r"\s+", " ", s).strip()

def run_embr(embr, flags, script, workdir):
    f = pathlib.Path(workdir) / "block.embr"
    f.write_text(script)
    env = dict(os.environ, EMBR_PATH=str(pathlib.Path(embr).parent))
    return subprocess.run([embr, *flags, str(f)], capture_output=True, text=True, cwd=workdir, env=env, timeout=60)

def check_blocks(doc, embr, include, backends):
    hello = (root / "examples/hello/hello.cpp").read_text()
    hello_norm = {norm(l) for l in hello.split("\n")}
    items = list(blocks(doc))
    for n, (lang, text, line) in enumerate(items):
        where = f"{doc.name}:{line}"
        if lang in ("embr-run", "embr-run-vm"):
            expect = items[n + 1][1] if n + 1 < len(items) and items[n + 1][0] == "output" else None
            for name, flags in backends:
                if lang == "embr-run-vm" and name != "vm":
                    continue
                with tempfile.TemporaryDirectory() as d:
                    r = run_embr(embr, flags, text, d)
                if r.returncode != 0:
                    fail(where, f"[{name}] exited {r.returncode}: {(r.stderr or r.stdout).strip()[:300]}")
                elif expect is not None and r.stdout != expect:
                    fail(where, f"[{name}] output differs\n--- expected\n{expect}--- got\n{r.stdout}")
        elif lang == "cpp-compile":
            with tempfile.TemporaryDirectory() as d:
                f = pathlib.Path(d) / "block.cpp"
                f.write_text(text)
                r = subprocess.run(["g++", "-std=c++20", "-fsyntax-only", "-I", include, str(f)],
                                   capture_output=True, text=True)
            if r.returncode != 0:
                fail(where, "does not compile:\n" + r.stderr[:600])
        elif lang == "cpp-check":
            for l in text.split("\n"):
                if norm(l) and norm(l) not in hello_norm:
                    fail(where, f"line not found in examples/hello/hello.cpp: {norm(l)}")

def check_numbers(wiki):
    def page(name):
        f = wiki / f"{name}.md"
        if not f.exists():
            fail(name, "wiki page not found")
            return ""
        return f.read_text()
    lex, overview = page("lexical-grammar"), page("overview")
    scoping, types = page("scoping-and-closures"), page("type-system")
    lexer = (root / "include/embr/core/lexer.h").read_text()
    kw = re.findall(r'\{"(\w+)",TokenType::', lexer[lexer.index("keywordTable()"):lexer.index("class Lexer")])
    m = re.search(r"### reserved keywords \((\d+)\)", lex)
    if not m or int(m.group(1)) != len(kw):
        fail("lexical-grammar", f"says {m.group(1) if m else '?'} reserved keywords, lexer has {len(kw)}")
    mo = re.search(r"(\d+) reserved keywords", overview)
    if mo and int(mo.group(1)) != len(kw):
        fail("overview", f"says {mo.group(1)} keywords, lexer has {len(kw)}")
    block = next((t for lang, t, _ in blocks_of(lex) if t.startswith("if  elif")), "")
    if set(block.split()) != set(kw):
        fail("lexical-grammar", f"keyword list differs from the lexer: {sorted(set(block.split()) ^ set(kw))}")
    registry = (root / "include/embr/core/registry.h").read_text()
    depth = re.search(r"maxCallDepth_\s*=\s*(\d+)", registry).group(1)
    if f"max call depth ({depth})" not in scoping:
        fail("scoping-and-closures", f"call depth message does not mention the real default {depth}")
    value = (root / "include/embr/core/value.h").read_text()
    nest = re.search(r"kMaxValueDepth\s*=\s*(\d+)", value).group(1)
    if f"max depth {nest})" not in types:
        fail("type-system", f"nesting limit does not mention the real cap {nest}")

def check_pages(wiki):
    """every plugin and script module has a page, a list entry and an index row."""
    def text(name):
        f = wiki / f"{name}.md"
        return f.read_text() if f.exists() else ""
    reference, modules, index = text("plugin-reference"), text("script-modules"), text("import-name-index")
    for d in sorted((root / "plugins").iterdir()):
        if not ((d / f"{d.name}.cpp").exists() or (d / f"{d.name}.c").exists()):
            continue
        n = d.name
        if n == "hello":
            continue
        if not (wiki / f"plugin-{n}.md").exists():
            fail(f"plugin-{n}", "plugins/%s has no wiki page" % n)
        if f"](plugin-{n})" not in reference:
            fail("plugin-reference", f"does not list `{n}`")
        if f"| `{n}` |" not in index:
            fail("import-name-index", f"has no row for `{n}`")
    for f in sorted((root / "modules").glob("*.embr")):
        n = f.stem
        if not (wiki / f"module-{n}-embr.md").exists():
            fail(f"module-{n}-embr", f"modules/{f.name} has no wiki page")
        if f"](module-{n}-embr)" not in modules:
            fail("script-modules", f"does not list `{n}.embr`")
        if f"| `{n}.embr` |" not in index:
            fail("import-name-index", f"has no row for `{n}.embr`")

CAPS_WORD = {"filesystem": "filesystem", "network": "network", "process": "process", "env": "environment",
             "threads": "thread", "native": "native"}

def page_words(page):
    """the page text, plus the names a shorthand like `math_sqrt/pow/sin` stands for, and a regex that finds a whole name."""
    # a table may write several names with a shared start as `math_sqrt/pow/sin`: that names math_sqrt, math_pow, math_sin
    for m in re.finditer(r"([a-z][a-z0-9_]*_)([a-z0-9]+(?:/[a-z0-9_]+)+)", page):
        for tail in m.group(2).split("/"):
            page += f" {m.group(1)}{tail} "
    return page

def names_on_page(where, names, groups, page, allow_calls=("len", "has")):
    """every name is on the page, and every function-looking name in the first column of a table is one of `names`."""
    words = lambda s: re.compile(r"(?<![A-Za-z0-9_])" + re.escape(s) + r"(?![A-Za-z0-9_])")
    page = page_words(page)
    known = set(names) | set(g for g in groups.values() if g)
    for n in names:
        if n.startswith("__"):
            continue                       # a hook the language calls (len(v), v[i]), not a function a script names
        if not words(n).search(page) and not (groups.get(n) and words(groups[n]).search(page)):
            fail(where, f"the page never names `{n}`")
    # the other way: a function-looking name in the first column of a table must be a real function
    for line in page.split("\n"):
        if not line.startswith("| ") or line.startswith("|---"):
            continue
        first = line.split("|")[1] if line.count("|") >= 2 else ""
        for tick in re.findall(r"`([^`]*)`", first):
            for m in re.finditer(r"(?<![A-Za-z0-9_/.])([a-z_][a-z0-9_]*)\(", tick):
                if m.group(1) not in known and m.group(1) not in allow_calls:
                    fail(where, f"the page lists `{m.group(1)}(...)` but it does not exist")

def capabilities_on_page(where, caps, page):
    for cap in caps:
        if CAPS_WORD[cap] not in page.lower():
            fail(where, f"it uses the {cap} capability but the page never says so")

def check_plugin_metadata(embr, wiki):
    """what a plugin says about itself (embr plugin NAME --json) against its wiki page."""
    import json
    ref_file = wiki / "plugin-reference.md"
    reference = ref_file.read_text() if ref_file.exists() else ""
    for d in sorted((root / "plugins").iterdir()):
        n = d.name
        if not ((d / f"{n}.cpp").exists() or (d / f"{n}.c").exists()) or n == "hello":
            continue
        where = f"plugin-{n}"
        run = subprocess.run([embr, "plugin", n, "--json"], capture_output=True, text=True)
        if run.returncode != 0:
            fail(where, f"`embr plugin {n} --json` failed: {(run.stderr or run.stdout).strip()[:300]}")
            continue
        info = json.loads(run.stdout)
        if not info.get("declared"):
            fail(where, "the plugin does not describe itself (EMBR_PLUGIN_INFO / embr_plugin_info)")
        if not info.get("version"):
            fail(where, "the plugin has no version")
        if len(info.get("description", "")) < 8:
            fail(where, "the plugin has no description")
        page_file = wiki / f"plugin-{n}.md"
        page = page_file.read_text() if page_file.exists() else ""
        for f in info["functions"]:
            if not f["bound"]:
                fail(where, f"the plugin documents `{f['name']}` but does not bind it")
            if not f["doc"].strip():
                fail(where, f"`{f['name']}` has no doc line in the plugin's description")
        names_on_page(where, [f["name"] for f in info["functions"]], {f["name"]: f["group"] for f in info["functions"]}, page)
        capabilities_on_page(where, info["capabilities"] + info["optional_capabilities"], page)
        # the list on plugin-reference says what the plugin needs, in brackets after its line
        line = next((l for l in reference.split("\n") if f"](plugin-{n})" in l), "")
        m = re.search(r"\(needs ([^)]*)\)", line)
        listed = sorted(c.strip() for c in m.group(1).split(",")) if m else []
        if listed != sorted(info["capabilities"]):
            fail("plugin-reference", f"the line for `{n}` says it needs {listed or 'nothing'}, the plugin says {sorted(info['capabilities']) or 'nothing'}")
        if info["optional_capabilities"] and "when allowed" not in line:
            fail("plugin-reference", f"the line for `{n}` does not say it uses {info['optional_capabilities']} when allowed")

def check_module_metadata(embr, wiki):
    """what a script module says about itself (embr module FILE --json) against its wiki page."""
    import json
    for f in sorted((root / "modules").glob("*.embr")):
        n = f.stem
        where = f"module-{n}-embr"
        run = subprocess.run([embr, "module", str(f), "--json"], capture_output=True, text=True)
        if run.returncode != 0 and not run.stdout.strip():
            fail(where, f"`embr module {f.name} --json` failed: {(run.stderr or run.stdout).strip()[:300]}")
            continue
        info = json.loads(run.stdout)
        if not info.get("declared"):
            fail(where, f"modules/{f.name} has no `__module__` declaration")
        if not info.get("version"):
            fail(where, "the module declares no version")
        if len(info.get("description", "")) < 8:
            fail(where, "the module declares no description")
        for p in info.get("problems", []):
            fail(where, f"the module has a problem: {p}")
        for fn in info["functions"]:
            if not fn["doc"].strip():
                fail(where, f"`{fn['name']}` has no comment above it to use as a doc line")
        page_file = wiki / f"module-{n}-embr.md"
        page = page_file.read_text() if page_file.exists() else ""
        names_on_page(where, [fn["name"] for fn in info["functions"]], {}, page)
        capabilities_on_page(where, info["capabilities"], page)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--embr", required=True)
    ap.add_argument("--include", required=True, help="directory that holds embr/embr.h")
    default_wiki = os.environ.get("EMBR_WIKI_DIR", "") or (str(root / "wiki") if (root / "wiki" / "home.md").exists() else "")
    ap.add_argument("--wiki", default=default_wiki, help="a clone of the wiki repo (default: the wiki/ submodule)")
    a = ap.parse_args()
    if not a.wiki:
        print("no wiki found (--wiki DIR, EMBR_WIKI_DIR, or git submodule update --init), skipping the docs checks")
        sys.exit(77)
    wiki = pathlib.Path(a.wiki).resolve()
    if not (wiki / "home.md").exists():
        print(f"{wiki} does not look like a wiki clone (no home.md)")
        sys.exit(1)
    if not (wiki / "plugin-reference.md").exists():
        print(f"{wiki} is an old copy of the wiki (no plugin-reference.md), pull it: git -C {wiki} pull")
        sys.exit(77)
    a.embr = str(pathlib.Path(a.embr).resolve())
    a.include = str(pathlib.Path(a.include).resolve())
    version = subprocess.run([a.embr, "--version"], capture_output=True, text=True).stdout
    # "embr 1.0.0 (build/linux-debug; backends: vm)" -> every listed backend runs every example
    names = [n.strip() for n in version.split("backends:")[-1].rstrip(")\n ").split(",") if n.strip()]
    backends = [(n, [f"--backend={n}"]) for n in names]
    for doc in sorted(wiki.glob("*.md")):
        check_blocks(doc, a.embr, a.include, backends)
    check_numbers(wiki)
    check_pages(wiki)
    check_plugin_metadata(a.embr, wiki)
    check_module_metadata(a.embr, wiki)
    if failures:
        print("\n\n".join(failures)); sys.exit(1)
    print("docs examples ok")

main()
