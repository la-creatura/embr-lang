// grammar.js
// a tree-sitter grammar for embr, for editors that use tree-sitter (Neovim, Helix, Zed). it follows include/embr/core/parser.h:
// statements need no separator (newlines mean nothing), blocks end with `end`, `elif`/`else` chain inside one `if`.
//
// to build it: `npm install` here, then `npx tree-sitter generate`. `tools/editor/tree-sitter/check.sh` parses every .embr file
// in the repo with it and fails on a syntax error, so the grammar cannot drift from the parser.

const PREC = { or: 1, and: 2, compare: 3, bit_or: 4, bit_xor: 5, bit_and: 6, shift: 7, add: 8, mul: 9, unary: 10, call: 11 };

module.exports = grammar({
  name: 'embr',

  extras: $ => [/\s/, $.comment],
  word: $ => $.identifier,

  conflicts: $ => [
    [$.fn_assign, $._expression],
    [$.map_pair, $._expression],
    [$._argument, $.parenthesized_expression],
    [$.binary_expression],
  ],

  rules: {
    source_file: $ => repeat($._statement),

    // `# ...` to the end of the line, and `#[ ... ]#` which does not nest
    comment: _ => token(choice(/#\[([^\]]|\]+[^\]#])*\]+#/, /#([^\[\n][^\n]*)?/)),

    _statement: $ => choice(
      $.if_statement, $.while_statement, $.for_statement, $.try_statement,
      $.break_statement, $.continue_statement, $.return_statement, $.import_statement,
      $.local_statement, $.const_statement, $.const_import_statement, $.c_text_statement, $.function_definition, $.fn_assign,
      $.multi_assign, $.assignment, $.compound_assignment, $.expression_statement,
    ),

    _block: $ => repeat1($._statement),

    if_statement: $ => seq(
      'if', field('condition', $._expression), field('then', optional($._block)),
      repeat($.elif_clause), optional($.else_clause), 'end',
    ),
    elif_clause: $ => seq('elif', field('condition', $._expression), optional($._block)),
    else_clause: $ => seq('else', optional($._block)),

    while_statement: $ => seq('while', field('condition', $._expression), optional($._block), 'end'),

    for_statement: $ => seq(
      'for', field('key', $.identifier), optional(seq(',', field('value', $.identifier))), 'in',
      field('iterable', $._expression), optional($._block), 'end',
    ),

    try_statement: $ => seq('try', optional($._block), 'catch', field('error', $.identifier), optional($._block), 'end'),

    break_statement: _ => 'break',
    continue_statement: _ => 'continue',
    return_statement: $ => seq('return', $._expression),
    import_statement: $ => seq('import', field('path', $._expression)),

    // local x = 1   local int x = 1   local arr<int> xs = []   local a, b = f()   local [a, b] = f()   local int [a, b] = f()
    local_statement: $ => seq(
      'local',
      choice(
        seq(optional($._type), '[', commaSep1($.identifier), ']'),
        commaSep1($.local_target),
      ),
      '=', $._expression,
    ),
    local_target: $ => seq(optional($._type), $.identifier),

    const_statement: $ => choice(
      seq('const', field('name', $.identifier), '=', field('value', $._expression)),
      seq('const', $.function_definition),
    ),

    // const import "file.embr"   const c import "file.c"   const c file import "file.h"   (the file is read where it is written). one token, so that
    // `const c = 1` and a variable called `c` are still read as before
    const_import_statement: $ => seq(
      alias(token(seq('const', optional(seq(/[ \t]+/, 'c', optional(seq(/[ \t]+/, 'file')))), /[ \t]+/, 'import')), $.const_import_keyword),
      field('path', $.string),
    ),

    // c """ ... """   c file """ ... """   : C text for aembr, the compiler. `embr` refuses a file that has one
    c_text_statement: $ => token(seq('c', optional(seq(/[ \t]+/, 'file')), /[ \t]*/, '"""', /([^"]|"[^"]|""[^"])*/, '"""')),

    function_definition: $ => seq(
      'fn', field('name', $.identifier), '(', optional($.parameters), ')', optional($.return_type),
      optional($._block), 'end',
    ),

    // sq(x) = x * x
    fn_assign: $ => seq(
      field('name', $.identifier), '(', optional($.parameters), ')', optional($.return_type), '=', field('body', $._expression),
    ),

    parameters: $ => commaSep1($.parameter),
    // fn f(a, b: int = 5, &c: arr<int>, v: host): a default and `&name` (in-out) are for aembr, the compiler; `host` is the type of a plugin's host value
    parameter: $ => choice(
      seq('...', $.identifier, optional($.type_annotation)),
      seq(optional('&'), $.identifier, optional($.type_annotation), optional(seq('=', choice($.number, $.string, seq('-', $.number))))),
    ),
    type_annotation: $ => seq(':', $._type),
    return_type: $ => seq('->', $._type),
    _type: $ => choice(
      $.function_type,
      seq(alias($.identifier, $.type_name), optional(seq('<', $._type, '>'))),
    ),
    // fn(int, str) -> int : types only, and the result is optional. `-> fn(int) -> int` reads to the right
    function_type: $ => prec.right(seq('fn', '(', optional(commaSep1($._type)), ')', optional(seq('->', $._type)))),

    multi_assign: $ => seq($.identifier, repeat1(seq(',', $.identifier)), '=', $._expression),
    assignment: $ => seq(field('target', $._expression), '=', field('value', $._expression)),
    compound_assignment: $ => seq(field('target', $.identifier), field('operator', choice('+=', '-=', '*=', '/=')), field('value', $._expression)),
    expression_statement: $ => $._expression,

    _expression: $ => choice(
      $.binary_expression, $.unary_expression, $.call_expression, $.index_expression, $.member_expression,
      $.parenthesized_expression, $.array, $.map, $.lambda, $.identifier, $.number, $.string,
    ),

    binary_expression: $ => choice(
      prec.left(PREC.or, seq($._expression, 'or', $._expression)),
      prec.left(PREC.and, seq($._expression, 'and', $._expression)),
      prec.left(PREC.compare, seq($._expression, choice('==', '!=', '<', '>', '<=', '>='), $._expression)),
      prec.left(PREC.bit_or, seq($._expression, '|', $._expression)),
      prec.left(PREC.bit_xor, seq($._expression, '^', $._expression)),
      prec.left(PREC.bit_and, seq($._expression, '&', $._expression)),
      // `>>` is two `>` in the lexer (so `arr<arr<int>>` closes two types), and the parser reads a `>` right after a `>` as a shift
      prec.left(PREC.shift, seq($._expression, choice('<<', seq('>', '>')), $._expression)),
      prec.left(PREC.add, seq($._expression, choice('+', '-'), $._expression)),
      prec.left(PREC.mul, seq($._expression, choice('*', '/', '%'), $._expression)),
    ),
    unary_expression: $ => prec(PREC.unary, seq(choice('-', '!', '~'), $._expression)),

    call_expression: $ => prec.dynamic(10, prec(PREC.call, seq(field('function', $._expression), '(', optional(commaSep1($._argument)), ')'))),
    _argument: $ => choice($.reference, $._expression),
    reference: $ => seq('&', $._expression),
    index_expression: $ => prec(PREC.call, seq(field('object', $._expression), '[', field('index', $._expression), ']')),
    member_expression: $ => prec(PREC.call, seq(field('object', $._expression), '.', field('property', $.identifier))),
    parenthesized_expression: $ => seq('(', $._expression, ')'),

    // [1, 2, 3]   [0...10]   [0, 2...10]
    array: $ => seq('[', optional(choice(
      seq($._expression, '...', $._expression),
      seq($._expression, ',', $._expression, '...', $._expression),
      seq(commaSep1($._expression), optional(',')),
    )), ']'),
    map: $ => seq('{', optional(seq(commaSep1($.map_pair), optional(','))), '}'),
    map_pair: $ => seq(field('key', choice($.identifier, $._expression)), ':', field('value', $._expression)),

    lambda: $ => seq('fn', '(', optional($.parameters), ')', optional($.return_type), optional($._block), 'end'),

    identifier: _ => /[A-Za-z_][A-Za-z0-9_]*/,
    number: _ => token(choice(/0[xX][0-9a-fA-F]+/, /0[bB][01]+/, /[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?/)),
    string: _ => /"([^"\\]|\\(.|\n))*"/,
  },
});

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}
