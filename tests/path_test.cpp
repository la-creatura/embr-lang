// unit tests for plugins/path/path.cpp (pure string utilities, POSIX separators)

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {
        { "path_join: joins components, later absolute wins, empty skipped",
          "print(path_join(\"a\", \"b\", \"c.txt\"))\n"
          "print(path_join(\"a\", \"/abs\", \"x\"))\n"
          "print(path_join(\"a\", \"\", \"b\"))\n"
          "print(path_join(\"only\"))\n",
          "a/b/c.txt\n/abs/x\na/b\nonly\n" },

        { "path_dirname / path_basename: including trailing slash and root",
          "print(path_dirname(\"/x/y.txt\"))\nprint(path_basename(\"/x/y.txt\"))\n"
          "print(path_dirname(\"a/b/\"))\nprint(path_basename(\"a/b/\"))\n"
          "print(path_dirname(\"file\"))\nprint(path_basename(\"/\"))\n",
          "/x\ny.txt\na\nb\n\n\n" },

        { "path_extname / path_stem: last extension only, dotfiles have none",
          "print(path_extname(\"/x/y.tar.gz\"))\nprint(path_stem(\"/x/y.tar.gz\"))\n"
          "print(path_extname(\".bashrc\"))\nprint(path_extname(\"noext\"))\n",
          ".gz\ny.tar\n\n\n" },

        { "path_normalize: collapses . .. and repeated slashes lexically",
          "print(path_normalize(\"a/./b/../c\"))\nprint(path_normalize(\"/a//b/\"))\n"
          "print(path_normalize(\"../x\"))\nprint(path_normalize(\"\"))\nprint(path_normalize(\"a/..\"))\n",
          "a/c\n/a/b\n../x\n.\n.\n" },

        { "path_is_abs / path_relative",
          "print(path_is_abs(\"/x\"))\nprint(path_is_abs(\"x\"))\n"
          "print(path_relative(\"/a/b/c\", \"/a\"))\nprint(path_relative(\"/a/b\", \"/a/b\"))\n",
          "1\n0\nb/c\n.\n" },

        // the answers are the ones std::filesystem gave (the first version of this plugin was built on it). these are the odd corners
        { "path: repeated separators, a trailing separator, a root with two slashes",
          "print(path_join(\"//\", \"x\"))\nprint(path_join(\"/\", \"/x\"))\nprint(path_join(\"a//b//\", \"\"))\n"
          "print(path_join(\"a//b\", \"c///d/\"))\nprint(path_dirname(\"//x/y\"))\nprint(path_dirname(\"a//b\"))\n"
          "print(path_relative(\"a/b/\", \"a\"))\nprint(path_relative(\"a\", \"a/b\"))\n"
          "print(path_normalize(\"a/b/..\"))\nprint(path_normalize(\"/../a\"))\n",
          "//x\n/x\na/b/\na/b/c/d/\n/x\na\nb/\n..\na\n/a\n" },
    };
    return embr_test::runSuite("path plugin test suite", tests, {"path"});
}
