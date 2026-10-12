// unit tests for plugins/fs/fs.cpp (imported once by main() below, plus embrlib for write_file() to make fixtures)
//
// the join tests don't assert the exact string because fs_join's separator depends on the platform. they round-trip
// through fs_basename/fs_dirname, which take either separator. the basename/dirname/extname tests use a "/" path,
// which std::filesystem reads the same on every platform we build for
//
// every test uses its own fixture name under the shared CTest working directory (BIN_DIR), since all tests in this
// suite share one Interpreter and cwd, and removes what it made with fs_remove

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "fs_basename / fs_dirname / fs_extname",
          "print(fs_basename(\"a/b/c.txt\"))\n"
          "print(fs_dirname(\"a/b/c.txt\"))\n"
          "print(fs_extname(\"a/b/c.txt\"))\n"
          "print(fs_extname(\"noext\"))\n",
          "c.txt\na/b\n.txt\n\n" },

        { "fs_join: round-trips through basename/dirname regardless of platform separator",
          "p = fs_join(\"a\", \"b\", \"c\")\n"
          "print(fs_basename(p))\n"
          "print(fs_dirname(fs_dirname(p)))\n",
          "c\na\n" },

        // the answers are the ones std::filesystem gave (the first version of this plugin was built on it). these are the odd corners
        { "fs_join / fs_dirname / fs_basename: repeated separators, a trailing separator, a root",
          "print(fs_join(\"a//b//\"))\nprint(fs_join(\"a\", \"\"))\nprint(fs_dirname(\"a/b/\"))\nprint(\"[\" + fs_basename(\"a/b/\") + \"]\")\n"
          "print(fs_dirname(\"//\"))\nprint(\"[\" + fs_dirname(\"a\") + \"]\")\nprint(\"[\" + fs_extname(\"a.b/c\") + \"]\")\n",
          "a/b/\na/\na/b\n[]\n/\n[]\n[]\n" },

        { "fs_join: requires at least one argument",
          "try\nfs_join()\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "fs_mkdir: creates a directory, is idempotent, fs_is_dir reflects it",
          "r1 = fs_mkdir(\"test_fs_dir1\")\n"
          "r2 = fs_mkdir(\"test_fs_dir1\")\n"
          "print(r1)\nprint(r2)\nprint(fs_is_dir(\"test_fs_dir1\"))\n"
          "fs_remove(\"test_fs_dir1\")\n",
          "1\n0\n1\n" },

        { "fs_mkdir: recursive creates missing parent directories",
          "print(fs_mkdir(\"test_fs_dir2/nested/deep\", 1))\n"
          "print(fs_is_dir(\"test_fs_dir2/nested/deep\"))\n"
          "fs_remove(\"test_fs_dir2/nested/deep\")\n"
          "fs_remove(\"test_fs_dir2/nested\")\n"
          "fs_remove(\"test_fs_dir2\")\n",
          "1\n1\n" },

        { "fs_exists / fs_remove: file lifecycle, removing twice returns 0 the second time",
          "write_file(\"test_fs_file1.txt\", \"x\")\n"
          "print(fs_exists(\"test_fs_file1.txt\"))\n"
          "print(fs_is_dir(\"test_fs_file1.txt\"))\n"
          "print(fs_remove(\"test_fs_file1.txt\"))\n"
          "print(fs_exists(\"test_fs_file1.txt\"))\n"
          "print(fs_remove(\"test_fs_file1.txt\"))\n",
          "1\n0\n1\n0\n0\n" },

        { "fs_remove: raises on a non-empty directory instead of deleting its contents",
          "fs_mkdir(\"test_fs_dir3\")\n"
          "write_file(\"test_fs_dir3/inner.txt\", \"x\")\n"
          "try\nfs_remove(\"test_fs_dir3\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n"
          "print(fs_is_dir(\"test_fs_dir3\"))\n"
          "fs_remove(\"test_fs_dir3/inner.txt\")\n"
          "fs_remove(\"test_fs_dir3\")\n",
          "caught\n1\n" },

        { "fs_list_dir: lists entries sorted alphabetically",
          "fs_mkdir(\"test_fs_listdir\")\n"
          "write_file(\"test_fs_listdir/b.txt\", \"x\")\n"
          "write_file(\"test_fs_listdir/a.txt\", \"x\")\n"
          "write_file(\"test_fs_listdir/c.txt\", \"x\")\n"
          "print(fs_list_dir(\"test_fs_listdir\"))\n"
          "fs_remove(\"test_fs_listdir/a.txt\")\n"
          "fs_remove(\"test_fs_listdir/b.txt\")\n"
          "fs_remove(\"test_fs_listdir/c.txt\")\n"
          "fs_remove(\"test_fs_listdir\")\n",
          "[\"a.txt\", \"b.txt\", \"c.txt\"]\n" },

        { "fs_list_dir: raises on a path that isn't a directory",
          "try\nfs_list_dir(\"test_fs_does_not_exist\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },
    };
    return embr_test::runSuite("fs plugin test suite", tests, {"embrlib", "fs"});
}
