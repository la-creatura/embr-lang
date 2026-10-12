; highlighting for embr. node names come from grammar.js

(comment) @comment
(string) @string
(number) @number

[
  "if" "elif" "else" "while" "for" "in" "try" "catch" "end" "return"
  "break" "continue" "import" "local" "const" "fn"
] @keyword

["and" "or"] @keyword.operator

["+" "-" "*" "/" "%" "==" "!=" "<" ">" "<=" ">=" "=" "+=" "-=" "*=" "/=" "!" "&" "|" "^" "~" "<<" "->" "..."] @operator
["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ":" "."] @punctuation.delimiter

(type_name) @type

(function_definition name: (identifier) @function)
(fn_assign name: (identifier) @function)
(call_expression function: (identifier) @function.call)
(call_expression function: (member_expression property: (identifier) @function.call))
(parameter (identifier) @variable.parameter)
(member_expression property: (identifier) @property)
(map_pair key: (identifier) @property)
(for_statement key: (identifier) @variable)
(try_statement error: (identifier) @variable)

(identifier) @variable
