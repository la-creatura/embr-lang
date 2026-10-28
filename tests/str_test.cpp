// unit tests for plugins/str/str.cpp (imported once by main() below)

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "str_split: comma separated",
          "print(str_split(\"a,b,c\", \",\"))\n",
          "[\"a\", \"b\", \"c\"]\n" },

        { "str_split: empty separator splits into characters",
          "print(str_split(\"abc\", \"\"))\n",
          "[\"a\", \"b\", \"c\"]\n" },

        // replacing inside one string shifted the tail on every match: a 3 MB string with 280,000 matches took many seconds
        { "str_replace: many matches in a long string stay linear",
          "s = str_repeat(\"The quick brown fox, jumps over the lazy dog. \", 70000)\n"
          "r = str_replace(s, \"fox\", \"wolf\")\n"
          "print(str_count(r, \"wolf\"))\nprint(str_count(r, \"fox\"))\nprint(str_find(r, \"wolf, jumps\"))\n"
          "print(str_endswith(r, \"lazy dog. \"))\n",
          "70000\n0\n16\n1\n" },

        { "str_replace: replaces every occurrence",
          "print(str_replace(\"a-b-c\", \"-\", \"_\"))\n",
          "a_b_c\n" },

        { "str_strip: trims both ends",
          "print(str_strip(\"  hi  \"))\n",
          "hi\n" },

        { "str_lower / str_upper",
          "print(str_lower(\"HeLLo\"))\nprint(str_upper(\"HeLLo\"))\n",
          "hello\nHELLO\n" },

        { "str_cmp: byte order, usable with sort_cmp",
          "print(str_cmp(\"a\", \"b\"))\nprint(str_cmp(\"b\", \"a\"))\nprint(str_cmp(\"ab\", \"ab\"))\nprint(str_cmp(\"ab\", \"abc\"))\nprint(str_cmp(\"Z\", \"a\"))\n"
          "print(sort_cmp([\"pear\", \"apple\", \"fig\"], str_cmp))\n",
          "-1\n1\n0\n-1\n-1\n[\"apple\", \"fig\", \"pear\"]\n" },

        { "str_find: returns index of first match",
          "print(str_find(\"hello\", \"ll\"))\n",
          "2\n" },

        { "str_find: -1 when not found",
          "print(str_find(\"hello\", \"zz\"))\n",
          "-1\n" },

        { "str_startswith / str_endswith",
          "print(str_startswith(\"hello\", \"he\"))\nprint(str_endswith(\"hello\", \"lo\"))\n"
          "print(str_startswith(\"hello\", \"lo\"))\n",
          "1\n1\n0\n" },

        { "str_isdigit / str_isalpha / str_isalnum",
          "print(str_isdigit(\"123\"))\nprint(str_isalpha(\"abc\"))\nprint(str_isalnum(\"abc123\"))\n"
          "print(str_isdigit(\"12a\"))\n",
          "1\n1\n1\n0\n" },

        { "str_count: counts non-overlapping occurrences",
          "print(str_count(\"banana\", \"a\"))\nprint(str_count(\"banana\", \"na\"))\n",
          "3\n2\n" },

        { "str_ord / str_chr round trip",
          "print(str_ord(\"A\"))\nprint(str_chr(65))\nprint(str_chr(str_ord(\"z\")))\n",
          "65\nA\nz\n" },

        { "str_repeat: repeats n times, 0 gives empty string",
          "print(str_repeat(\"ab\", 3))\nprint(str_repeat(\"x\", 0))\nprint(str_repeat(\"x\", 0) == \"\")\n",
          "ababab\n\n1\n" },

        { "str_reverse",
          "print(str_reverse(\"hello\"))\n",
          "olleh\n" },

        { "str_pad_left / str_pad_right",
          "print(str_pad_left(\"7\", 4, \"0\"))\nprint(str_pad_right(\"hi\", 5, \".\"))\n"
          "print(str_pad_left(\"longer\", 3))\n",  // already >= width: unchanged
          "0007\nhi...\nlonger\n" },

        { "str_ltrim / str_rtrim: one-sided trims",
          "print(str_ltrim(\"  hi  \") == \"hi  \")\nprint(str_rtrim(\"  hi  \") == \"  hi\")\n",
          "1\n1\n" },

        { "str_repeat / str_pad / str_chr: huge, negative and NaN-ish counts raise cleanly instead of overflowing",
          "big = 1.5\ni = 0\nwhile i < 40\nbig = big * 1000.0\ni = i + 1\nend\n"
          "try\n  str_repeat(\"ab\", 4000000000000)\n  print(\"unreachable\")\ncatch e\n  print(\"repeat capped\")\nend\n"
          "try\n  str_repeat(\"ab\", big)\n  print(\"unreachable\")\ncatch e\n  print(\"repeat huge capped\")\nend\n"
          "try\n  str_pad_left(\"a\", 100000000000, \"x\")\n  print(\"unreachable\")\ncatch e\n  print(\"pad capped\")\nend\n"
          "try\n  str_chr(big)\n  print(\"unreachable\")\ncatch e\n  print(\"chr capped\")\nend\n"
          "print(str_repeat(\"ab\", 3))\nprint(str_pad_left(\"7\", 3, \"0\"))\nprint(str_find(\"abc\", \"c\", -5))\n",
          "repeat capped\nrepeat huge capped\npad capped\nchr capped\nababab\n007\n2\n" },

        { "str_format: placeholders, explicit indices, brace escapes",
          "print(str_format(\"{} scored {}\", \"ada\", 97))\n"
          "print(str_format(\"{1}-{0} {0}\", \"a\", \"b\"))\n"
          "print(str_format(\"{{literal}} {}\", 5))\n"
          "print(str_format(\"no placeholders\"))\n"
          "print(str_format(\"{} {} {}\", [1, \"x\"], {\"k\": 2}, 2.5))\n",
          "ada scored 97\nb-a a\n{literal} 5\nno placeholders\n[1, \"x\"] {k: 2} 2.5\n" },

        { "str_format: width, alignment and fill for strings and numbers",
          "print(str_format(\"[{:>6}] [{:<6}] [{:^6}]\", \"a\", \"b\", \"c\"))\n"
          "print(str_format(\"[{:*^7}] [{:->5}] [{:.<4}]\", \"mid\", 42, \"x\"))\n"
          "print(str_format(\"[{:6}] [{:6}]\", \"ab\", 12))\n"
          "print(str_format(\"[{:.3}]\", \"abcdef\"))\n"
          "print(str_format(\"[{:2}]\", \"toolong\"))\n",
          "[     a] [b     ] [  c   ]\n[**mid**] [---42] [x...]\n[ab    ] [    12]\n[abc]\n[toolong]\n" },

        { "str_format: integer types, signs, zero padding, prefixes",
          "print(str_format(\"{:d} {:+d} {: d} {:05d} {:+05d}\", 42, 42, 42, 42, 42))\n"
          "print(str_format(\"{:d} {:05d}\", -7, -7))\n"
          "print(str_format(\"{:x} {:X} {:o} {:b}\", 255, 255, 8, 5))\n"
          "print(str_format(\"{:#x} {:#X} {:#o} {:#b} {:#010x}\", 255, 255, 8, 5, 255))\n"
          "print(str_format(\"{:d}\", 3.0))\n"
          "print(str_format(\"{:x}\", -255))\n"
          "print(str_format(\"{:d}\", -9223372036854775807 - 1))\n",
          "42 +42  42 00042 +0042\n-7 -0007\nff FF 10 101\n0xff 0XFF 0o10 0b101 0x000000ff\n3\n-ff\n-9223372036854775808\n" },

        { "str_format: floats (f e g, precision, sign, zero padding, nan/inf) and natural number form",
          "print(str_format(\"{:f} {:.2f} {:.0f} {:8.3f} {:08.3f} {:+.1f}\", 3.14159, 3.14159, 2.5, 3.14159, 3.14159, 2))\n"
          "print(str_format(\"{:e} {:.2e} {:g} {:.3g}\", 12345.678, 12345.678, 0.0001, 1234.5678))\n"
          "print(str_format(\"{:f} {:d} {:.1f}\", 5, 5, -0.04))\n"
          "print(str_format(\"{} {} {} {:.3}\", 7 / 2, 1e3, -2.5, 3.14159))\n"
          "print(str_format(\"[{:>8}] [{:+}] [{:08}]\", 3.5, 4, -12))\n",
          "3.141590 3.14 2    3.142 0003.142 +2.0\n1.234568e+04 1.23e+04 0.0001 1.23e+03\n5.000000 5 -0.0\n3.5 1000 -2.5 3.14\n[     3.5] [+4] [-0000012]\n" },

        { "str_format: errors (too few arguments, bad specs, mixed numbering, bad types, stray braces)",
          "try\n  str_format(\"{} {}\", 1)\n  print(\"unreachable\")\ncatch e\n  print(\"too few\")\nend\n"
          "try\n  str_format(\"{} {0}\", 1)\n  print(\"unreachable\")\ncatch e\n  print(\"mixed\")\nend\n"
          "try\n  str_format(\"{:q}\", 1)\n  print(\"unreachable\")\ncatch e\n  print(\"bad type\")\nend\n"
          "try\n  str_format(\"{:d}\", 1.5)\n  print(\"unreachable\")\ncatch e\n  print(\"d needs integer\")\nend\n"
          "try\n  str_format(\"{:d}\", \"x\")\n  print(\"unreachable\")\ncatch e\n  print(\"d needs number\")\nend\n"
          "try\n  str_format(\"{:f}\", \"x\")\n  print(\"unreachable\")\ncatch e\n  print(\"f needs number\")\nend\n"
          "try\n  str_format(\"{\", 1)\n  print(\"unreachable\")\ncatch e\n  print(\"unterminated\")\nend\n"
          "try\n  str_format(\"}\", 1)\n  print(\"unreachable\")\ncatch e\n  print(\"stray close\")\nend\n"
          "try\n  str_format(\"{:99999999}\", 1)\n  print(\"unreachable\")\ncatch e\n  print(\"width capped\")\nend\n"
          "print(str_format(\"extra args are ignored\", 1, 2, 3))\n",
          "too few\nmixed\nbad type\nd needs integer\nd needs number\nf needs number\nunterminated\nstray close\nwidth capped\nextra args are ignored\n" },

        { "num_fixed: fixed digits, rounding, range check",
          "print(num_fixed(3.14159, 2))\nprint(num_fixed(2, 3))\nprint(num_fixed(2.5, 0))\nprint(num_fixed(-0.001, 2))\n"
          "try\n  num_fixed(1, 101)\n  print(\"unreachable\")\ncatch e\n  print(\"digits capped\")\nend\n",
          "3.14\n2.000\n2\n-0.00\ndigits capped\n" },

        { "str_repeat: an empty string repeated a huge number of times returns at once",
          "print(str_repeat(\"\", 4000000000000) == \"\")\n",
          "1\n" },
    };
    return embr_test::runSuite("str plugin test suite", tests, {"str", "embrlib"});
}
