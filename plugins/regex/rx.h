/* rx.h
 * the regular-expression engine of the regex plugin, in plain C and independent of embr, so tests/regex_engine_test.cpp
 * can test it alone.
 *
 * a pattern is compiled to a small program. most patterns run on a Pike VM: linear time, no recursion over the subject, no
 * backtracking. a pattern with a backreference (\1, \k<name>) or a lookahead ((?= (?!) runs on a backtracking matcher
 * with an explicit stack instead, bounded by a step limit. see the wiki, plugin-regex.
 *
 * syntax (ECMAScript subset, bytes not characters): literals . [classes] \d \D \w \W \s \S \b \B \n \r \t \f \v \0 \xHH
 * \uHHHH, anchors ^ $, groups ( ) (?: ) (?<name> ), alternation, * + ? {n} {n,} {n,m} and their lazy forms,
 * backreferences, lookahead. lookbehind is refused.
 */
#ifndef EMBR_REGEX_RX_H
#define EMBR_REGEX_RX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RX_MAX_PATTERN 20000     /* bytes of pattern text */
#define RX_MAX_INSTS   200000    /* compiled program size (counted quantifiers expand a copy per repeat) */
#define RX_MAX_DEPTH   200       /* nesting of groups */
#define RX_MAX_REPEAT  1000      /* largest {n} / {n,m} bound */
#define RX_MAX_STEPS   50000000L /* the backtracking matcher gives up after this many steps */

typedef struct RxProg RxProg;
typedef struct RxMatcher RxMatcher;

enum { RX_OK = 0, RX_SYNTAX = 1, RX_NOMEM = 2 };

/* compiles a pattern. returns NULL on failure with a message in err (and the reason in *code) */
RxProg* rx_compile(const char* pat, size_t n, int multiline, char* err, size_t errcap, int* code);
void    rx_free(RxProg* p);

int         rx_ngroups(const RxProg* p);            /* capture groups, not counting the whole match */
const char* rx_group_name(const RxProg* p, int g);   /* "" when group g has no name */
/* NULL when the pattern runs on the Pike VM, else a few words on what needs the backtracking matcher */
const char* rx_backtrack_reason(const RxProg* p);

/* a matcher keeps its buffers between searches. the subject must outlive it */
RxMatcher* rx_matcher_new(const RxProg* p, const char* s, size_t n);
void       rx_matcher_free(RxMatcher* m);
/* the leftmost match starting at or after `from`. caps gets 2 * (ngroups + 1) offsets, -1 for a group that did not take
   part. returns 1 for a match, 0 for none, -1 when it gave up (out of memory, or too many steps) */
int        rx_search(RxMatcher* m, size_t from, int* caps);

#ifdef __cplusplus
}
#endif
#endif
