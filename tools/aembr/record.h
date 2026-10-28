// record.h
// the record every output of aembr carries: what made it, so it can be read without running it. a program or a plugin holds it as text
// inside the file (found by scanning, so `aembr info` never opens a library), and a package holds it as `aembr/build.json`.
//
// usage
//   BuildRecord r; r.kind = "plugin"; ...
//   std::string json = recordJson(r);                 // one line, the same text for the same record
//   BuildRecord back; parseRecord(json, back);
//   std::string found; findRecord(fileBytes, found);  // the text of a record inside a program or a plugin
//
// good to know
//   - nothing in it depends on where or when the file was made: no dates, no folders, only the name of the source file
//   - `flags` is what the C compiler was given that changes the program (not the folders it looked in)

#ifndef AEMBR_RECORD_H
#define AEMBR_RECORD_H

#include <string>
#include <vector>

namespace aembr {

struct BuildRecord {
    std::string              aembr;           // the version of aembr
    std::string              kind;            // program or plugin
    std::string              name;            // a plugin's name
    std::string              source;          // the source file's name
    std::string              sourceSha256;
    std::vector<std::string> capabilities;    // what the program needs, worked out from its source and the plugins it calls
    std::string              cc;              // the first line of `cc --version`
    std::string              flags;
    std::string              runtimeSha256;   // the runtime library and headers it was built with
    std::string              target;          // linux-x86_64
};

std::string recordJson(const BuildRecord& r);
bool        parseRecord(const std::string& json, BuildRecord& out);
// the text a record is written between inside a program or a plugin
std::string recordBegin();
std::string recordEnd();
bool        findRecord(const std::string& fileBytes, std::string& json);

}  // namespace aembr

#endif
