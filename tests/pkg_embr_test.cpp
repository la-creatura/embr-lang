// unit tests for modules/pkg.embr
//
// CTest has no guaranteed network, so installs use a registry kept in a folder (file://, see REG_SETUP below): the
// project file, the lockfile, deps and the checksum checks run for real. the http download path is checked by hand
// against a local server
//
// tests that touch disk use their own fixture names in the suite's working directory

#include "harness.h"

// a registry kept in a folder (registry_url file://...), so installs can be tested without a network. each test passes
// its own folder, and leftovers of an earlier run are removed first
#define REG_SETUP \
    "fn reg_setup(d)\n" \
    "    fs_mkdir(d + \"/reg/a\", 1)\n" \
    "    fs_mkdir(d + \"/proj\", 1)\n" \
    "    for f in [\"proj/embr.json\", \"proj/embr.lock\", \"proj/installed.json\", \"proj/modules/greet.embr\", \"proj/modules/helper.embr\", \"proj/cache/greet-1.0.0.embr\", \"proj/cache/greet-2.0.0.embr\", \"proj/cache/helper-1.0.0.embr\", \"proj/cache/helper-2.0.0.embr\"]\n" \
    "        if fs_exists(d + \"/\" + f)\n" \
    "            fs_remove(d + \"/\" + f)\n" \
    "        end\n" \
    "    end\n" \
    "    write_file(d + \"/reg/a/greet-2.0.0.embr\", \"import \\\"helper.embr\\\"\\nfn greet(n)\\n    return \\\"hello \\\" + n + helper()\\nend\\n\")\n" \
    "    write_file(d + \"/reg/a/greet-1.0.0.embr\", \"fn greet(n)\\n    return \\\"hi \\\" + n\\nend\\n\")\n" \
    "    write_file(d + \"/reg/a/helper-2.0.0.embr\", \"fn helper()\\n    return \\\" (h2)\\\"\\nend\\n\")\n" \
    "    write_file(d + \"/reg/a/helper-1.0.0.embr\", \"fn helper()\\n    return \\\" (h1)\\\"\\nend\\n\")\n" \
    "    g2 = http_sha256(load_file(d + \"/reg/a/greet-2.0.0.embr\"))\n" \
    "    idx = {\"greet\": {\"latest\": \"2.0.0\", \"versions\": {\n" \
    "        \"1.0.0\": {\"url\": \"/a/greet-1.0.0.embr\", \"type\": \"embr\"},\n" \
    "        \"2.0.0\": {\"url\": \"/a/greet-2.0.0.embr\", \"type\": \"embr\", \"sha256\": g2, \"deps\": {\"helper\": \"2.0.0\"}}}},\n" \
    "        \"helper\": {\"versions\": {\"1.0.0\": {\"url\": \"/a/helper-1.0.0.embr\", \"type\": \"embr\"}, \"2.0.0\": {\"url\": \"/a/helper-2.0.0.embr\", \"type\": \"embr\"}}}}\n" \
    "    write_file(d + \"/reg/index.json\", json_stringify(idx))\n" \
    "    p = d + \"/proj/\"\n" \
    "    pkg_configure({\"registry_url\": \"file://\" + d + \"/reg\", \"project_path\": p + \"embr.json\", \"lock_path\": p + \"embr.lock\",\n" \
    "                   \"manifest_path\": p + \"installed.json\", \"cache_dir\": p + \"cache\", \"modules_dir\": p + \"modules\"})\n" \
    "    return p\n" \
    "end\n"


// a registry with a module that needs the network (its file imports http), one that needs nothing, and a native plugin
// artifact (a dummy file: it is never opened). cap_setup(d, claimed, claim): when claimed is 1 the registry says what each
// needs (claim for the first and the plugin), otherwise it says nothing
static const char* CAP_SETUP = R"EMBR(
fn cap_setup(d, claimed, claim)
    fs_mkdir(d + "/reg/a", 1)
    fs_mkdir(d + "/proj", 1)
    for f in ["proj/embr.json", "proj/embr.lock", "proj/installed.json", "proj/modules/netpkg.embr", "proj/modules/pure.embr", "proj/plugins/plug.so", "proj/cache/netpkg-1.0.0.embr", "proj/cache/pure-1.0.0.embr", "proj/cache/plug-1.0.0.so"]
        if fs_exists(d + "/" + f)
            fs_remove(d + "/" + f)
        end
    end
    write_file(d + "/reg/a/netpkg-1.0.0.embr", "import \"http\"\nfn get()\n    return 1\nend\n")
    write_file(d + "/reg/a/pure-1.0.0.embr", "fn pure()\n    return 1\nend\n")
    write_file(d + "/reg/a/plug-1.0.0.so", "not really a plugin")
    local net = {"url": "/a/netpkg-1.0.0.embr", "type": "embr", "description": "fetches things"}
    local plug = {"url": "/a/plug-1.0.0.so", "type": "plugin"}
    if claimed == 1
        net["capabilities"] = claim
        plug["capabilities"] = claim
    end
    local idx = {"netpkg": {"versions": {"1.0.0": net}}, "pure": {"versions": {"1.0.0": {"url": "/a/pure-1.0.0.embr", "type": "embr"}}}, "plug": {"versions": {"1.0.0": plug}}}
    write_file(d + "/reg/index.json", json_stringify(idx))
    local p = d + "/proj/"
    pkg_configure({"registry_url": "file://" + d + "/reg", "project_path": p + "embr.json", "lock_path": p + "embr.lock",
                   "manifest_path": p + "installed.json", "cache_dir": p + "cache", "modules_dir": p + "modules",
                   "plugins_dir": p + "plugins", "deny_caps": []})
    return p
end
)EMBR";

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "pkg_versions: returns versions in ascending semver order",
          "idx = {\"foo\": {\"versions\": {\"2.0.0\": {}, \"1.0.0\": {}, \"1.5.2\": {}}}}\n"
          "print(pkg_versions(idx, \"foo\"))\n",
          "[\"1.0.0\", \"1.5.2\", \"2.0.0\"]\n" },

        { "pkg_versions: raises on an unknown package",
          "idx = {}\n"
          "try\npkg_versions(idx, \"nope\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "pkg_resolve: \"latest\" prefers the index's own latest field when present",
          "idx = {\"foo\": {\"latest\": \"1.0.0\", \"versions\": {\"1.0.0\": {}, \"2.0.0\": {}}}}\n"
          "print(pkg_resolve(idx, \"foo\", \"latest\"))\n",
          "1.0.0\n" },

        { "pkg_resolve: \"latest\" falls back to the highest semver version without a latest field",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}, \"1.9.0\": {}, \"1.10.0\": {}}}}\n"
          "print(pkg_resolve(idx, \"foo\", \"latest\"))\n",
          "1.10.0\n" },

        { "pkg_resolve: an empty spec behaves like \"latest\"",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}}}}\n"
          "print(pkg_resolve(idx, \"foo\", \"\"))\n",
          "1.0.0\n" },

        { "pkg_resolve: an exact version spec is returned as-is when present",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}, \"2.0.0\": {}}}}\n"
          "print(pkg_resolve(idx, \"foo\", \"1.0.0\"))\n",
          "1.0.0\n" },

        { "pkg_resolve: raises on a version spec the registry doesn't have",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}}}}\n"
          "try\npkg_resolve(idx, \"foo\", \"9.9.9\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "pkg_resolve: \">=X.Y.Z\" picks the lowest satisfying version, not the highest",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}, \"1.5.0\": {}, \"2.0.0\": {}}}}\n"
          "print(pkg_resolve(idx, \"foo\", \">=1.2.0\"))\n",
          "1.5.0\n" },

        { "pkg_resolve: \">=X.Y.Z\" raises when nothing satisfies it",
          "idx = {\"foo\": {\"versions\": {\"1.0.0\": {}}}}\n"
          "try\npkg_resolve(idx, \"foo\", \">=5.0.0\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "pkg_fetch_index: raises without a configured registry",
          "try\npkg_fetch_index()\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "pkg_remove: raises on a package that was never installed",
          "pkg_configure({\"manifest_path\": \"test_pkg_manifest_empty.json\"})\n"
          "try\npkg_remove(\"nope\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "pkg_list / manifest round-trip: reads back what pkg_save_manifest wrote",
          "pkg_configure({\"manifest_path\": \"test_pkg_manifest1.json\"})\n"
          "pkg_save_manifest({\"foo\": {\"version\": \"1.0.0\", \"type\": \"embr\", \"path\": \"modules/foo.embr\"}})\n"
          "m = pkg_list()\n"
          "print(m[\"foo\"][\"version\"])\n"
          "print(m[\"foo\"][\"type\"])\n"
          "fs_remove(\"test_pkg_manifest1.json\")\n",
          "1.0.0\nembr\n" },

        { "__pkg_install_dir: routes plugin/embr/anything-else to plugins_dir/modules_dir/vendor_dir",
          "print(__pkg_install_dir(\"plugin\"))\n"
          "print(__pkg_install_dir(\"embr\"))\n"
          "print(__pkg_install_dir(\"asset\"))\n",
          "plugins\nmodules\nvendor\n" },

        { "__pkg_extname: keeps double-barrelled archive suffixes intact instead of truncating to the last dot",
          "print(__pkg_extname(\"https://github.com/o/r/archive/refs/tags/v1.0.0.tar.gz\"))\n"
          "print(__pkg_extname(\"https://example.com/x.tar.bz2\"))\n"
          "print(__pkg_extname(\"https://example.com/x.tar.xz\"))\n"
          "print(__pkg_extname(\"https://example.com/x.TAR.GZ\"))\n"
          "print(__pkg_extname(\"https://example.com/foo-1.0.0.so\"))\n"
          "print(__pkg_extname(\"https://example.com/foo-1.0.0.embr\"))\n"
          "print(__pkg_extname(\"https://example.com/noext\"))\n",
          ".tar.gz\n.tar.bz2\n.tar.xz\n.TAR.GZ\n.so\n.embr\n\n" },

        { "pkg_list: an unwritten manifest reads back as empty",
          "pkg_configure({\"manifest_path\": \"test_pkg_manifest_missing.json\"})\n"
          "m = pkg_list()\n"
          "print(len(keys(m)))\n",
          "0\n" },

        { "pkg_main: \"list\" with nothing installed",
          "pkg_configure({\"manifest_path\": \"test_pkg_manifest_main_empty.json\"})\n"
          "pkg_main([\"list\"])\n",
          "no packages installed\n" },

        { "pkg_main: unknown command prints usage and returns 1",
          "r = pkg_main([\"bogus\"])\n"
          "print(r)\n",
          "usage: embr pkg [--registry <url>] <command> [args]\n"
          "commands:\n"
          "  install <name> [version]   install a package (default: latest) and add it to embr.json\n"
          "  install                    install everything embr.json lists (from embr.lock when it is up to date)\n"
          "  list                       list installed packages\n"
          "  remove <name>              uninstall a package\n"
          "  versions <name>            list a package's available versions\n"
          "  info <name> [version]      what the registry says about a package: type, dependencies, capabilities\n"
          "  info --verify <name> [version]   the same, and what the downloaded file itself needs (nothing is installed)\n"
          "  pins                       the signer each installed package is pinned to\n"
          "options: --registry <url>, --deny-caps=a,b  refuse a package that needs one of these capabilities\n"
          "         (filesystem, network, process, env, threads, native)\n"
          "         --require-signed  install only packages signed by a key you trust (embr trust)\n"
          "         --allow-signer-change  accept a package whose signer is not the one it was installed with\n"
          "1\n" },

        { "pkg_main: install without a registry configured reports a caught error, not a crash",
          "pkg_configure({\"registry_url\": \"\"})\n"
          "r = pkg_main([\"install\", \"foo\"])\n"
          "print(r)\n",
          "pkg error: pkg: no registry configured -- call pkg_configure({\"registry_url\": \"http://host:port\"}) first\n"
          "1\n" },
        { "pkg_install: a package from a folder registry comes with its deps, and goes into embr.json and embr.lock at exact versions",
          REG_SETUP
          "p = reg_setup(\"t_pkg_deps\")\n"
          "info = pkg_install(\"greet\", \"latest\")\n"
          "print(info[\"version\"])\n"
          "print(fs_exists(p + \"modules/greet.embr\"), fs_exists(p + \"modules/helper.embr\"))\n"
          "print(json_parse(load_file(p + \"embr.json\"))[\"dependencies\"])\n"
          "lock = json_parse(load_file(p + \"embr.lock\"))\n"
          "print(lock[\"project\"])\n"
          "print(lock[\"packages\"][\"helper\"][\"version\"], lock[\"packages\"][\"greet\"][\"url\"], len(lock[\"packages\"][\"greet\"][\"sha256\"]))\n",
          "2.0.0\n1 1\n{greet: \"2.0.0\"}\n{greet: \"2.0.0\"}\n2.0.0 /a/greet-2.0.0.embr 64\n" },
        { "pkg_install_project: with an up-to-date lock it installs what the lock says, without reading the registry index",
          REG_SETUP
          "p = reg_setup(\"t_pkg_lock\")\n"
          "pkg_install(\"greet\", \"2.0.0\")\n"
          "fs_remove(p + \"modules/greet.embr\")\n"
          "fs_remove(p + \"modules/helper.embr\")\n"
          "fs_remove(\"t_pkg_lock/reg/index.json\")\n"
          "names = sort_do(pkg_install_project(), fn(s) return len(s) end)\n"
          "print(names)\n"
          "print(fs_exists(p + \"modules/greet.embr\"), fs_exists(p + \"modules/helper.embr\"))\n",
          "[\"greet\", \"helper\"]\n1 1\n" },
        { "pkg_install_project: a changed embr.json is resolved again and gets a new lock",
          REG_SETUP
          "p = reg_setup(\"t_pkg_relock\")\n"
          "pkg_install(\"greet\", \"2.0.0\")\n"
          "write_file(p + \"embr.json\", json_stringify({\"dependencies\": {\"greet\": \"1.0.0\"}}))\n"
          "print(pkg_install_project())\n"
          "print(json_parse(load_file(p + \"embr.lock\"))[\"project\"])\n",
          "[\"greet\"]\n{greet: \"1.0.0\"}\n" },
        { "pkg_resolve_all: the same package at two versions is an error naming both, a non-exact dependency too",
          REG_SETUP
          "reg_setup(\"t_pkg_conflict\")\n"
          "idx = pkg_fetch_index()\n"
          "try\n"
          "    pkg_resolve_all(idx, {\"greet\": \"2.0.0\", \"helper\": \"1.0.0\"})\n"
          "catch e\n"
          "    print(e[\"message\"])\n"
          "end\n"
          "idx[\"greet\"][\"versions\"][\"2.0.0\"][\"deps\"] = {\"helper\": \"latest\"}\n"
          "try\n"
          "    pkg_resolve_all(idx, {\"greet\": \"2.0.0\"})\n"
          "catch e\n"
          "    print(e[\"message\"])\n"
          "end\n",
          "pkg: 'helper' is needed at 1.0.0 by the project and at 2.0.0 by greet@2.0.0\npkg: greet@2.0.0 depends on 'helper' at 'latest': a dependency must name an exact version\n" },
        { "pkg_install_project: a download that doesn't match the lock's sha256 is refused",
          REG_SETUP
          "p = reg_setup(\"t_pkg_tamper\")\n"
          "pkg_install(\"greet\", \"2.0.0\")\n"
          "fs_remove(p + \"cache/greet-2.0.0.embr\")\n"
          "write_file(\"t_pkg_tamper/reg/a/greet-2.0.0.embr\", \"fn greet(n)\\n    return \\\"changed\\\"\\nend\\n\")\n"
          "try\n"
          "    pkg_install_project()\n"
          "catch e\n"
          "    print(slice(e[\"message\"], 0, 38))\n"
          "end\n",
          "pkg: checksum mismatch for greet@2.0.0\n" },
        { "pkg_remove: drops the package from embr.json and deletes embr.lock; install with no embr.json says what to do",
          REG_SETUP
          "p = reg_setup(\"t_pkg_remove\")\n"
          "pkg_install(\"greet\", \"1.0.0\")\n"
          "pkg_remove(\"greet\")\n"
          "print(json_parse(load_file(p + \"embr.json\"))[\"dependencies\"])\n"
          "print(fs_exists(p + \"embr.lock\"))\n"
          "fs_remove(p + \"embr.json\")\n"
          "print(pkg_main([\"install\"]))\n",
          "{}\n0\npkg error: pkg: no t_pkg_remove/proj/embr.json here: add a package with `embr pkg install <name>` first\n1\n" },

        { "pkg_install: a package that needs a denied capability is refused before anything is installed",
          std::string(CAP_SETUP) +
          "p = cap_setup(\"t_pkg_deny\", 0, [])\n"
          "pkg_configure({\"deny_caps\": [\"network\"]})\n"
          "try\n    pkg_install(\"netpkg\", \"1.0.0\")\ncatch e\n    print(e[\"message\"])\nend\n"
          "print(fs_exists(p + \"modules/netpkg.embr\"))\nprint(fs_exists(p + \"embr.json\"))\n",
          "pkg: netpkg@1.0.0 needs network, which is denied\n0\n0\n" },

        { "pkg_install: the same package installs when the capability is not denied; under a deny list a module that needs nothing installs, and the project's other package is re-checked",
          std::string(CAP_SETUP) +
          "p = cap_setup(\"t_pkg_allow\", 0, [])\n"
          "pkg_install(\"netpkg\", \"1.0.0\")\nprint(fs_exists(p + \"modules/netpkg.embr\"))\n"
          "p = cap_setup(\"t_pkg_allow\", 0, [])\n"          // a fresh project: installing re-checks every package the project lists
          "pkg_configure({\"deny_caps\": [\"network\", \"process\", \"threads\"]})\n"
          "pkg_install(\"pure\", \"1.0.0\")\nprint(fs_exists(p + \"modules/pure.embr\"))\n"
          "try\n    pkg_install(\"netpkg\", \"1.0.0\")\ncatch e\n    print(e[\"message\"])\nend\n",
          "1\n1\npkg: netpkg@1.0.0 needs network, which is denied\n" },

        { "pkg_install: a registry that claims less than the file needs is caught by reading the file",
          std::string(CAP_SETUP) +
          "p = cap_setup(\"t_pkg_lie\", 1, [\"network\"])\n"
          "try\n    pkg_install(\"netpkg\", \"1.0.0\")\ncatch e\n    print(e[\"message\"])\nend\n"
          "print(fs_exists(p + \"modules/netpkg.embr\"))\n",
          "pkg: the registry says netpkg@1.0.0 needs [network] but the file also needs filesystem\n0\n" },

        { "pkg_install: a registry claim that covers the file installs it",
          std::string(CAP_SETUP) +
          "p = cap_setup(\"t_pkg_honest\", 1, [\"filesystem\", \"network\"])\n"
          "pkg_install(\"netpkg\", \"1.0.0\")\nprint(fs_exists(p + \"modules/netpkg.embr\"))\n",
          "1\n" },

        { "pkg_install: a native plugin is never opened, so under a deny list it needs a claim from the registry",
          std::string(CAP_SETUP) +
          "p = cap_setup(\"t_pkg_plugin\", 0, [])\n"
          "pkg_configure({\"deny_caps\": [\"network\"]})\n"
          "try\n    pkg_install(\"plug\", \"1.0.0\")\ncatch e\n    print(e[\"message\"])\nend\n"
          "print(fs_exists(p + \"plugins/plug.so\"))\n"
          "p = cap_setup(\"t_pkg_plugin\", 1, [\"threads\"])\n"
          "pkg_configure({\"deny_caps\": [\"network\"]})\n"
          "pkg_install(\"plug\", \"1.0.0\")\nprint(fs_exists(p + \"plugins/plug.so\"))\n"
          "p = cap_setup(\"t_pkg_plugin\", 1, [\"network\"])\n"
          "pkg_configure({\"deny_caps\": [\"network\"]})\n"
          "try\n    pkg_install(\"plug\", \"1.0.0\")\ncatch e\n    print(e[\"message\"])\nend\n",
          "pkg: plug@1.0.0 is a native plugin and the registry does not say what it needs, so it cannot be checked against --deny-caps\n0\n1\n"
          "pkg: plug@1.0.0 needs network, which is denied\n" },

        { "pkg_info: what the registry says about a package, without installing it",
          std::string(CAP_SETUP) +
          "p = cap_setup(\"t_pkg_info\", 1, [\"filesystem\", \"network\"])\n"
          "i = pkg_info(\"netpkg\", \"latest\")\n"
          "print(i[\"name\"], i[\"version\"], i[\"type\"], i[\"description\"], i[\"capabilities\"], i[\"claimed\"])\n"
          "p = cap_setup(\"t_pkg_info\", 0, [])\n"
          "i = pkg_info(\"pure\", \"1.0.0\")\nprint(i[\"capabilities\"], i[\"claimed\"])\nprint(fs_exists(p + \"modules/pure.embr\"))\n",
          "netpkg 1.0.0 embr fetches things [\"filesystem\", \"network\"] 1\n[] 0\n0\n" },

        { "pkg_main: --deny-caps= on the command line, and the info command",
          std::string(CAP_SETUP) +
          "p = cap_setup(\"t_pkg_cli\", 1, [\"filesystem\", \"network\"])\n"
          "print(pkg_main([\"--deny-caps=network,process\", \"install\", \"netpkg\"]))\n"
          "print(fs_exists(p + \"modules/netpkg.embr\"))\n"
          "print(pkg_main([\"info\", \"netpkg\"]))\n",
          "pkg error: pkg: netpkg@1.0.0 needs network, which is denied\n1\n0\n"
          "netpkg@1.0.0  (embr)\n  fetches things\n  url: /a/netpkg-1.0.0.embr\n  the registry says it needs: filesystem, network\n0\n" },

        { "pkg_verify: reads the downloaded file, installs nothing, and says what the registry over-claims",
          std::string(CAP_SETUP) +
          "p = cap_setup(\"t_pkg_verify\", 1, [\"filesystem\", \"network\", \"process\"])\n"
          "v = pkg_verify(\"netpkg\", \"1.0.0\")\n"
          "print(v[\"verified\"], v[\"file_capabilities\"], v[\"over_claimed\"], v[\"problems\"])\n"
          "print(fs_exists(p + \"modules/netpkg.embr\"))\n"
          "v = pkg_verify(\"plug\", \"1.0.0\")\nprint(v[\"verified\"], v[\"claimed\"])\n"
          "p = cap_setup(\"t_pkg_verify\", 1, [\"network\"])\n"
          "v = pkg_verify(\"netpkg\", \"1.0.0\")\nprint(v[\"capabilities\"], v[\"file_capabilities\"])\n",
          "1 [\"filesystem\", \"network\"] [\"process\"] []\n0\n0 1\n[\"network\"] [\"filesystem\", \"network\"]\n" },

        { "pkg_main: info --verify shows what the file needs and what the registry over-claims",
          std::string(CAP_SETUP) +
          "p = cap_setup(\"t_pkg_verify_cli\", 1, [\"filesystem\", \"network\", \"process\"])\n"
          "print(pkg_main([\"info\", \"--verify\", \"netpkg\"]))\n"
          "print(pkg_main([\"info\", \"--verify\", \"plug\"]))\n",
          "netpkg@1.0.0  (embr)\n  fetches things\n  url: /a/netpkg-1.0.0.embr\n  the registry says it needs: filesystem, network, process\n"
          "  the downloaded file needs: filesystem, network  (read without running it)\n  the registry claims more than the file needs: process\n0\n"
          "plug@1.0.0  (plugin)\n  url: /a/plug-1.0.0.so\n  the registry says it needs: filesystem, network, process\n"
          "  not verified: a native plugin is never opened before it is installed\n0\n" },
    };
    return embr_test::runSuite("pkg.embr module test suite", tests, {"pkg.embr"});
}
