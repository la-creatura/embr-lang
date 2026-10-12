// unit tests for plugins/encoding/encoding.c -- known-answer vectors (RFC 4648, FIPS 180, zlib crc32)

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {
        { "base64_encode: RFC 4648 vectors",
          "print(base64_encode(\"\"))\nprint(base64_encode(\"f\"))\nprint(base64_encode(\"fo\"))\n"
          "print(base64_encode(\"foo\"))\nprint(base64_encode(\"foob\"))\nprint(base64_encode(\"fooba\"))\nprint(base64_encode(\"foobar\"))\n",
          "\nZg==\nZm8=\nZm9v\nZm9vYg==\nZm9vYmE=\nZm9vYmFy\n" },

        { "base64: urlsafe alphabet and nopad round trip",
          "s = hex_decode(\"fbff\")\nprint(base64_encode(s))\nprint(base64_encode(s, 1))\nprint(base64_encode(s, 1, 1))\n"
          "print(hex_encode(base64_decode(\"-_8\", 1)))\n",
          "+/8=\n-_8=\n-_8\nfbff\n" },

        { "base64_decode: valid round trip, unpadded accepted",
          "print(base64_decode(\"Zm9vYmFy\"))\nprint(base64_decode(\"Zm9vYg==\"))\nprint(base64_decode(\"Zm9vYg\"))\n",
          "foobar\nfoob\nfoob\n" },

        { "base64_decode: malformed input raises",
          "try\nbase64_decode(\"Zm9v!mFy\")\nprint(\"unreachable\")\ncatch e\nprint(\"bad char\")\nend\n"
          "try\nbase64_decode(\"Zg=\")\nprint(\"unreachable\")\ncatch e\nprint(\"bad pad\")\nend\n"
          "try\nbase64_decode(\"Z\")\nprint(\"unreachable\")\ncatch e\nprint(\"bad len\")\nend\n"
          "try\nbase64_decode(\"Zh==\")\nprint(\"unreachable\")\ncatch e\nprint(\"non canonical\")\nend\n"
          "try\nbase64_decode(\"Zm9v\\n\")\nprint(\"unreachable\")\ncatch e\nprint(\"whitespace\")\nend\n",
          "bad char\nbad pad\nbad len\nnon canonical\nwhitespace\n" },

        { "hex_encode / hex_decode: round trip, both cases, errors",
          "print(hex_encode(\"hi\"))\nprint(hex_decode(\"6869\"))\nprint(hex_decode(\"6A6b\") == \"jk\")\n"
          "try\nhex_decode(\"abc\")\nprint(\"unreachable\")\ncatch e\nprint(\"odd\")\nend\n"
          "try\nhex_decode(\"zz\")\nprint(\"unreachable\")\ncatch e\nprint(\"digit\")\nend\n",
          "6869\nhi\n1\nodd\ndigit\n" },

        { "sha256: NIST vectors incl. multi-block",
          "print(sha256(\"\"))\nprint(sha256(\"abc\"))\n"
          "print(sha256(\"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq\"))\n"
          "print(len(sha256_raw(\"abc\")))\n",
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1\n32\n" },

        { "crc32: standard check value",
          "print(crc32(\"123456789\"))\nprint(crc32(\"\"))\n",
          "3421780262\n0\n" },
    };
    return embr_test::runSuite("encoding plugin test suite", tests, {"embrlib", "encoding"});
}
