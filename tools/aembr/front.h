// front.h
// reads an embr file for aembr: lex, parse and type check, keeping the type the checker found for every expression.
//
// usage
//   aembr::Front f;
//   if (!aembr::readFront(path, f, err)) { std::cerr << err; return 1; }
//   // f.ast is the program, f.types the checker's record (see typecheck.h, ExprTypes)

#ifndef AEMBR_FRONT_H
#define AEMBR_FRONT_H

#include <embr/embr.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace aembr {

struct Front {
    std::string                   path;
    std::string                   source;
    std::vector<embr::StmtPtr>    ast;
    embr::typecheck::ExprTypes    types;
    std::string                   stdDir;       // where the library written in aembr is (std/<plugin>.embr); empty: none is used
    std::map<const embr::Stmt*, std::string> stmtFile;   // the top level statements that came from an imported script module, and the file of each
    std::vector<std::string>      modules;      // the script modules the file imports, all of them, in the order they were read (for the build record)
    std::set<std::string>         stdDone;      // the library files already brought in
    std::set<const embr::Stmt*>   stdStmts;     // the top level statements that came from it (the compiler trusts them: C text there needs no `native`)
};

// `import "util.embr"` in the program, or in a module it imports, brings the module in: see readFrontText

// false: `err` holds the messages (a file that cannot be read, a syntax error, or the type errors), ready to print
bool readFront(const std::string& path, Front& out, std::string& err);
// the same for text that is already in `out.source` (with `out.path` naming it)
bool readFrontText(Front& out, std::string& err);

// a construct aembr does not compile (yet). `at` is where it is written
struct Refusal {
    std::string      msg;
    embr::SourceRange at;
    std::string      file;     // the imported script module it is written in; empty: the program's own file
};

}  // namespace aembr

#endif
