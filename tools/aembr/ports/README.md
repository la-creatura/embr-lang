# the C plugins, written again in aembr

`aencoding`, `apath`, `acsv` and `ajson` are the plugins `encoding`, `path`, `csv` and `json` written in aembr. each is built with `aembr plugin` and loaded with `import "aencoding"` and so on.
`tests/aembr_ports_smoke.sh` runs one script (`compare_<name>.embr`) with the C plugin and with the port and checks that the two print exactly the same, every error message and error kind included.

what is not the same: `urlsafe` and `nopad` of the base64 functions are ints (0 or 1), not any value that can be true; `json_stringify` takes the indent as a number (0 is compact, the default) and has no options map
(no `lossy`, no `strict_utf8`); `fs` is not ported, it resolves relative paths against the folder of the running script, which a plugin written in aembr cannot ask the host for yet.

how they are written: the loops over bytes (hex, base64, crc32, sha256, the csv scan and writer, the json scanner and writer) are C text over the string where it is. the aembr part checks the arguments, raises the errors (the C text only
says which and where) and has the optional parameters (`base64_encode(data, urlsafe = 0, nopad = 0)`, a csv `delimiter = ","`). `ajson` and `acsv` take and give the host's own values (`host`), so nothing is copied into a tree of
`any` values. `apath` is plain aembr with small C text (a byte of a string, a string from bytes).

speed: `STATS.md` has the table. `bench_<name>.embr` is the benchmark and `bench.sh NAME` builds the C plugin and the port with the same flags and runs it on both (use a release `embr`).

when a C plugin changes behavior, change the port, or the compare script says so.
