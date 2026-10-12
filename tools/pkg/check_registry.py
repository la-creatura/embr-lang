#!/usr/bin/env python3
# checks a package registry index (index.json) against tools/pkg/registry.schema.json, and the things a schema cannot say:
# latest is one of the package's versions, and every dependency names a package and version the index has.
#
#   check_registry.py INDEX.json            # exit 0 and nothing printed when it is fine, else one problem per line
#   check_registry.py --selftest            # runs the checker against the example index and some broken ones
#
# it reads the schema itself, so the schema file is the one place the rules live. only the parts of JSON Schema the file uses
# are understood (type, required, properties, additionalProperties, items, enum, pattern, minLength, minProperties,
# uniqueItems, propertyNames, $ref); anything else in the schema is a mistake in the schema and is reported as one.
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KNOWN = {"$schema", "title", "description", "type", "required", "properties", "additionalProperties", "items", "enum", "pattern",
         "minLength", "minProperties", "uniqueItems", "propertyNames", "$ref", "$defs"}
TYPES = {"object": dict, "array": list, "string": str}


def resolve(schema, root):
    while "$ref" in schema:
        ref = schema["$ref"]
        if not ref.startswith("#/"):
            raise ValueError("unsupported $ref " + ref)
        node = root
        for part in ref[2:].split("/"):
            node = node[part]
        schema = node
    return schema


def check(value, schema, root, where, out):
    schema = resolve(schema, root)
    for k in schema:
        if k not in KNOWN:
            out.append("%s: the schema uses '%s', which this checker does not understand" % (where, k))
    t = schema.get("type")
    if t and not isinstance(value, TYPES[t]):
        out.append("%s: expected %s" % (where, t))
        return
    if "enum" in schema and value not in schema["enum"]:
        out.append("%s: '%s' is not one of %s" % (where, value, ", ".join(schema["enum"])))
    if isinstance(value, str):
        if "pattern" in schema and not re.search(schema["pattern"], value):
            out.append("%s: '%s' does not match %s" % (where, value, schema["pattern"]))
        if len(value) < schema.get("minLength", 0):
            out.append("%s: must not be empty" % where)
    if isinstance(value, list):
        if schema.get("uniqueItems") and len(set(map(json.dumps, value))) != len(value):
            out.append("%s: has a repeated item" % where)
        if "items" in schema:
            for i, v in enumerate(value):
                check(v, schema["items"], root, "%s[%d]" % (where, i), out)
    if isinstance(value, dict):
        if len(value) < schema.get("minProperties", 0):
            out.append("%s: needs at least %d entries" % (where, schema["minProperties"]))
        for r in schema.get("required", []):
            if r not in value:
                out.append("%s: missing '%s'" % (where, r))
        props = schema.get("properties", {})
        for k, v in value.items():
            sub = "%s.%s" % (where, k) if where else k
            if "propertyNames" in schema:
                check(k, schema["propertyNames"], root, sub + " (name)", out)
            if k in props:
                check(v, props[k], root, sub, out)
            elif isinstance(schema.get("additionalProperties"), dict):
                check(v, schema["additionalProperties"], root, sub, out)


def semantic(index, out):
    for name, pkg in index.items():
        versions = pkg.get("versions") if isinstance(pkg, dict) else None
        if not isinstance(versions, dict):
            continue
        if "latest" in pkg and pkg["latest"] not in versions:
            out.append("%s: latest is %s, which is not in versions" % (name, pkg["latest"]))
        for ver, rel in versions.items():
            deps = rel.get("deps") if isinstance(rel, dict) else None
            if not isinstance(deps, dict):
                continue
            for dn, dv in deps.items():
                if dn not in index or not isinstance(index[dn], dict) or dv not in index[dn].get("versions", {}):
                    out.append("%s.%s: depends on %s@%s, which the index does not have" % (name, ver, dn, dv))


def validate(index):
    with open(os.path.join(HERE, "registry.schema.json")) as f:
        schema = json.load(f)
    out = []
    check(index, schema, schema, "", out)
    if isinstance(index, dict):
        semantic(index, out)
    return out


def selftest():
    with open(os.path.join(HERE, "example-index.json")) as f:
        good = json.load(f)
    problems = validate(good)
    if problems:
        print("selftest: the example index should be fine:", problems)
        return 1

    def broken(mutate, want):
        idx = json.loads(json.dumps(good))
        mutate(idx)
        got = validate(idx)
        if not any(want in p for p in got):
            print("selftest: expected a problem containing '%s', got %s" % (want, got))
            return 1
        return 0

    bad = 0
    bad += broken(lambda i: i["netpkg"]["versions"]["1.0.0"].pop("url"), "missing 'url'")
    bad += broken(lambda i: i["netpkg"].pop("versions"), "missing 'versions'")
    bad += broken(lambda i: i["netpkg"]["versions"]["1.0.0"].update(sha256="abc"), "does not match")
    bad += broken(lambda i: i["netpkg"]["versions"]["1.0.0"].update(capabilities=["internet"]), "is not one of")
    bad += broken(lambda i: i["netpkg"]["versions"]["1.0.0"].update(capabilities=["network", "network"]), "repeated item")
    bad += broken(lambda i: i["netpkg"].update(latest="9.9.9"), "not in versions")
    bad += broken(lambda i: i["netpkg"]["versions"]["1.1.0"].update(deps={"pure": "7.0.0"}), "does not have")
    bad += broken(lambda i: i["netpkg"]["versions"]["1.1.0"].update(deps={"ghost": "1.0.0"}), "does not have")
    bad += broken(lambda i: i["netpkg"]["versions"].update({"one": {"url": "/x"}}), "does not match")
    bad += broken(lambda i: i.update(pure=[]), "expected object")
    bad += broken(lambda i: i["pure"].update(versions={}), "at least 1")
    return 1 if bad else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(selftest())
    if len(sys.argv) != 2:
        print(__doc__.split("\n\n")[0], file=sys.stderr)
        sys.exit(2)
    try:
        with open(sys.argv[1]) as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        print("%s: %s" % (sys.argv[1], e))
        sys.exit(1)
    found = validate(data)
    for p in found:
        print(p)
    sys.exit(1 if found else 0)
