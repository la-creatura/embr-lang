// unit tests for plugins/csv/csv.c

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {
        { "csv_parse: plain rows, trailing newline adds no record, CRLF and bare CR",
          "r = csv_parse(\"a,b\\n1,2\\n\")\nprint(len(r))\nprint(r[1][1])\n"
          "r = csv_parse(\"a,b\\r\\n1,2\\r\\n3,4\")\nprint(len(r))\nprint(r[2][0])\n"
          "print(len(csv_parse(\"x\\ry\\rz\")))\nprint(len(csv_parse(\"\")))\n",
          "2\n2\n3\n3\n3\n0\n" },

        { "csv_parse: quoted fields (delimiter, newline, escaped quote, empty)",
          "r = csv_parse(\"\\\"a,b\\\",\\\"line1\\nline2\\\",\\\"say \\\"\\\"hi\\\"\\\"\\\",\\\"\\\"\\n\")\n"
          "print(len(r[0]))\nprint(r[0][0])\nprint(len(r[0][1]))\nprint(r[0][2])\nprint(len(r[0][3]))\n",
          "4\na,b\n11\nsay \"hi\"\n0\n" },

        { "csv_parse: empty fields, blank lines skipped, lone empty quoted field is a record",
          "r = csv_parse(\"a,,c\\n\\n,\\n\\\"\\\"\\n\")\nprint(len(r))\nprint(len(r[0]))\nprint(len(r[0][1]))\nprint(len(r[1]))\nprint(len(r[2]))\n"
          "",
          "3\n3\n0\n2\n1\n" },

        { "csv_parse: custom delimiter and bad delimiters",
          "print(csv_parse(\"a;b,c\", \";\")[0][1])\nprint(csv_parse(\"a\\tb\", \"\\t\")[0][1])\n"
          "try\ncsv_parse(\"a\", \"ab\")\nprint(\"unreachable\")\ncatch e\nprint(\"multi\")\nend\n"
          "try\ncsv_parse(\"a\", \"\\\"\")\nprint(\"unreachable\")\ncatch e\nprint(\"quote\")\nend\n",
          "b,c\nb\nmulti\nquote\n" },

        { "csv_parse: malformed input reports line and column",
          "try\ncsv_parse(\"a,b\\nc,\\\"oops\\n\")\nprint(\"unreachable\")\ncatch e\nprint(e[\"message\"])\nend\n"
          "try\ncsv_parse(\"a,b\\nc,d\\\"e\")\nprint(\"unreachable\")\ncatch e\nprint(e[\"message\"])\nend\n"
          "try\ncsv_parse(\"\\\"a\\\"b\")\nprint(\"unreachable\")\ncatch e\nprint(e[\"message\"])\nend\n",
          "unterminated quoted field opened at line 2, column 3\n"
          "unexpected quote inside an unquoted field at line 2, column 4\n"
          "unexpected character after closing quote at line 1, column 4\n" },

        { "csv_parse: huge input and many-quote input do not recurse or crash",
          "s = \"a,b\\n\"\ni = 0\nwhile i < 15\ns = s + s\ni = i + 1\nend\nprint(len(csv_parse(s)))\n",
          "32768\n" },

        { "csv_parse_dicts: header mapping and errors",
          "r = csv_parse_dicts(\"n,v\\nA,1\\nB,2\\n\")\nprint(len(r))\nprint(r[1][\"n\"])\nprint(r[1][\"v\"])\nprint(len(csv_parse_dicts(\"\")))\n"
          "try\ncsv_parse_dicts(\"a,b\\n1\\n\")\nprint(\"unreachable\")\ncatch e\nprint(\"ragged\")\nend\n"
          "try\ncsv_parse_dicts(\"a,a\\n1,2\\n\")\nprint(\"unreachable\")\ncatch e\nprint(\"dup\")\nend\n",
          "2\nB\n2\n0\nragged\ndup\n" },

        { "csv_stringify: quoting rules, numbers, round trip",
          "print(csv_stringify([[\"a\", 1, 2.5], [\"x,y\", \"q\\\"z\", \" pad\"]]))\n"
          "rows = [[\"a\\nb\", \"c,d\", \"\"], [\"\", \"\\\"\", \"e\"]]\nback = csv_parse(csv_stringify(rows))\n"
          "print(back[0][0] == \"a\\nb\")\nprint(back[0][1])\nprint(len(back[0][2]))\nprint(back[1][1])\n"
          "print(csv_stringify([[1, 2]], \";\"))\n",
          "a,1,2.5\n\"x,y\",\"q\"\"z\",\" pad\"\n\n1\nc,d\n0\n\"\n1;2\n\n" },

        { "csv_stringify: bad input raises",
          "try\ncsv_stringify([[1, [2]]])\nprint(\"unreachable\")\ncatch e\nprint(\"cell\")\nend\n"
          "try\ncsv_stringify([\"notarow\"])\nprint(\"unreachable\")\ncatch e\nprint(\"row\")\nend\n",
          "cell\nrow\n" },

        { "csv_stringify: a record that is one empty cell is quoted so it survives a round trip",
          "print(csv_stringify([[\"\"]]))\nprint(len(csv_parse(csv_stringify([[\"a\"], [\"\"], [\"b\"]]))))\n"
          "print(csv_stringify([[\"\", \"\"]]))\n",
          "\"\"\n\n3\n,\n\n" },
    };
    return embr_test::runSuite("csv plugin test suite", tests, {"embrlib", "csv"});
}
