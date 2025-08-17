#!/usr/bin/env python3
"""Tiny C front end targeting the Pscal VM.

This compiler implements a small subset of C described in ``Docs`` of this
repository.  It parses Tiny C source code and emits Pscal bytecode so that the
resulting program can be executed by the stand-alone ``pscalvm``.  Only integer
variables and functions are supported.  Calling VM builtins is enabled by simply
calling a function that lacks a corresponding definition.

Usage::

    python tools/tinycc.py source.c output.pbc

The output file can be executed with ``pscalvm``::

    ./build/bin/pscalvm output.pbc
"""
from __future__ import annotations

import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any

# ---------------------------------------------------------------------------
# Opcode loading
# ---------------------------------------------------------------------------

def load_opcodes(root: Path) -> Dict[str, int]:
    """Parse ``bytecode.h`` to recover opcode values."""
    header = root / "src" / "compiler" / "bytecode.h"
    pattern = re.compile(r"^\s*(OP_[A-Z0-9_]+)\s*,")
    opcodes: Dict[str, int] = {}
    enum_started = False
    index = 0
    with open(header, "r", encoding="utf-8") as f:
        for line in f:
            if not enum_started:
                if line.strip().startswith("typedef enum"):
                    enum_started = True
                continue
            if line.strip().startswith("}"):
                break
            m = pattern.search(line)
            if m:
                name = m.group(1)
                opcodes[name] = index
                index += 1
    return opcodes

# VarType enum (partial) -----------------------------------------------------
TYPE_INTEGER = 2
TYPE_STRING = 4

# ---------------------------------------------------------------------------
# Lexer
# ---------------------------------------------------------------------------

TOKEN_SPEC = [
    ("NUMBER", r"\d+"),
    ("ID", r"[A-Za-z_][A-Za-z0-9_]*"),
    ("EQ", r"=="),
    ("NE", r"!="),
    ("LE", r"<="),
    ("GE", r">="),
    ("OR", r"\|\|"),
    ("AND", r"&&"),
    ("ASSIGN", r"="),
    ("LT", r"<"),
    ("GT", r">"),
    ("PLUS", r"\+"),
    ("MINUS", r"-"),
    ("STAR", r"\*"),
    ("SLASH", r"/"),
    ("SEMI", r";"),
    ("COMMA", r","),
    ("LPAREN", r"\("),
    ("RPAREN", r"\)"),
    ("LBRACE", r"{"),
    ("RBRACE", r"}"),
    ("LBRACKET", r"\["),
    ("RBRACKET", r"\]"),
]

TOKEN_REGEX = re.compile("|".join(f"(?P<{name}>{regex})" for name, regex in TOKEN_SPEC))

KEYWORDS = {"int", "void", "if", "else", "while", "return"}

SYMBOL_MAP = {
    "EQ": "==",
    "NE": "!=",
    "LE": "<=",
    "GE": ">=",
    "OR": "||",
    "AND": "&&",
    "ASSIGN": "=",
    "LT": "<",
    "GT": ">",
    "PLUS": "+",
    "MINUS": "-",
    "STAR": "*",
    "SLASH": "/",
    "SEMI": ";",
    "COMMA": ",",
    "LPAREN": "(",
    "RPAREN": ")",
    "LBRACE": "{",
    "RBRACE": "}",
    "LBRACKET": "[",
    "RBRACKET": "]",
}

@dataclass
class Token:
    type: str
    value: str


def tokenize(source: str) -> List[Token]:
    tokens: List[Token] = []
    pos = 0
    while pos < len(source):
        m = TOKEN_REGEX.match(source, pos)
        if not m:
            if source[pos].isspace():
                pos += 1
                continue
            raise SyntaxError(f"Unexpected character {source[pos]!r}")
        name = m.lastgroup
        val = m.group(name)
        typ = SYMBOL_MAP.get(name, name)
        if typ == "ID" and val in KEYWORDS:
            typ = val.upper()
        tokens.append(Token(typ, val))
        pos = m.end()
    tokens.append(Token("EOF", ""))
    return tokens

# ---------------------------------------------------------------------------
# Parser AST nodes
# ---------------------------------------------------------------------------

@dataclass
class Node:
    pass

@dataclass
class Program(Node):
    decls: List[Node]

@dataclass
class VarDecl(Node):
    typ: str
    name: str

@dataclass
class Param(Node):
    typ: str
    name: str

@dataclass
class FunDecl(Node):
    typ: str
    name: str
    params: List[Param]
    body: "CompoundStmt"

@dataclass
class CompoundStmt(Node):
    vars: List[VarDecl]
    statements: List[Node]

@dataclass
class IfStmt(Node):
    cond: Node
    then_branch: Node
    else_branch: Optional[Node]

@dataclass
class WhileStmt(Node):
    cond: Node
    body: Node

@dataclass
class ReturnStmt(Node):
    expr: Optional[Node]

@dataclass
class ExprStmt(Node):
    expr: Optional[Node]

@dataclass
class Assign(Node):
    name: str
    expr: Node

@dataclass
class BinOp(Node):
    op: str
    left: Node
    right: Node

@dataclass
class Var(Node):
    name: str

@dataclass
class Num(Node):
    value: int

@dataclass
class Call(Node):
    name: str
    args: List[Node]

# ---------------------------------------------------------------------------
# Recursive descent parser
# ---------------------------------------------------------------------------

class Parser:
    def __init__(self, tokens: List[Token]):
        self.tokens = tokens
        self.pos = 0

    def current(self) -> Token:
        return self.tokens[self.pos]

    def consume(self, typ: str, msg: str) -> Token:
        tok = self.current()
        if tok.type != typ:
            raise SyntaxError(msg)
        self.pos += 1
        return tok

    def match(self, *types: str) -> bool:
        if self.current().type in types:
            self.pos += 1
            return True
        return False

    # program ::= declaration { declaration }
    def parse_program(self) -> Program:
        decls = []
        while self.current().type != "EOF":
            decls.append(self.parse_declaration())
        return Program(decls)

    # declaration ::= var_declaration | fun_declaration
    def parse_declaration(self) -> Node:
        if self.lookahead_is_var_decl():
            return self.parse_var_decl()
        else:
            return self.parse_fun_decl()

    def lookahead_is_var_decl(self) -> bool:
        # After type_specifier and identifier, if next token is ';' it's var decl
        save = self.pos
        self.parse_type_specifier()
        self.consume("ID", "Expected identifier")
        is_var = self.current().type == ";"
        self.pos = save
        return is_var

    # type_specifier ::= 'int' | 'void'
    def parse_type_specifier(self) -> Token:
        tok = self.current()
        if tok.type not in {"int".upper(), "void".upper()}:
            raise SyntaxError("Expected type specifier")
        self.pos += 1
        return tok

    # var_declaration ::= type_specifier identifier ';'
    def parse_var_decl(self) -> VarDecl:
        typ = self.consume("INT", "Expected type").value
        name = self.consume("ID", "Expected variable name").value
        self.consume(";", "Missing ';'")
        return VarDecl(typ, name)

    # fun_declaration ::= type_specifier identifier '(' params ')' compound_statement
    def parse_fun_decl(self) -> FunDecl:
        typ = self.consume(self.current().type, "Expected type").value
        name = self.consume("ID", "Expected function name").value
        self.consume("(", "Expected '('")
        params = self.parse_params()
        self.consume(")", "Expected ')'")
        body = self.parse_compound_stmt()
        return FunDecl(typ, name, params, body)

    # params ::= 'void' | param_list
    def parse_params(self) -> List[Param]:
        if self.match("VOID"):
            return []
        if self.current().type == ")":
            return []
        params = [self.parse_param()]
        while self.match(","):
            params.append(self.parse_param())
        return params

    # param ::= type_specifier identifier
    def parse_param(self) -> Param:
        typ = self.consume(self.current().type, "Expected type").value
        name = self.consume("ID", "Expected parameter name").value
        return Param(typ, name)

    # compound_statement ::= '{' { var_declaration } { statement } '}'
    def parse_compound_stmt(self) -> CompoundStmt:
        self.consume("{", "Expected '{'")
        vars: List[VarDecl] = []
        while self.current().type in {"INT", "VOID"}:
            vars.append(self.parse_var_decl())
        stmts: List[Node] = []
        while self.current().type != "}":
            stmts.append(self.parse_statement())
        self.consume("}", "Expected '}'")
        return CompoundStmt(vars, stmts)

    # statement ::= expression_statement | compound_statement | if_statement | while_statement | return_statement
    def parse_statement(self) -> Node:
        tok = self.current().type
        if tok == "IF":
            return self.parse_if()
        elif tok == "WHILE":
            return self.parse_while()
        elif tok == "RETURN":
            return self.parse_return()
        elif tok == "{":
            return self.parse_compound_stmt()
        else:
            return self.parse_expression_stmt()

    # if_statement ::= 'if' '(' expression ')' statement [ 'else' statement ]
    def parse_if(self) -> IfStmt:
        self.consume("IF", "Expected 'if'")
        self.consume("(", "Expected '(' after if")
        cond = self.parse_expression()
        self.consume(")", "Expected ')'")
        then_branch = self.parse_statement()
        else_branch = None
        if self.match("ELSE"):
            else_branch = self.parse_statement()
        return IfStmt(cond, then_branch, else_branch)

    # while_statement ::= 'while' '(' expression ')' statement
    def parse_while(self) -> WhileStmt:
        self.consume("WHILE", "Expected 'while'")
        self.consume("(", "Expected '(' after while")
        cond = self.parse_expression()
        self.consume(")", "Expected ')'")
        body = self.parse_statement()
        return WhileStmt(cond, body)

    # return_statement ::= 'return' [ expression ] ';'
    def parse_return(self) -> ReturnStmt:
        self.consume("RETURN", "Expected 'return'")
        if self.current().type == ";":
            self.consume(";", "Missing ';'")
            return ReturnStmt(None)
        expr = self.parse_expression()
        self.consume(";", "Missing ';'")
        return ReturnStmt(expr)

    # expression_statement ::= [ expression ] ';'
    def parse_expression_stmt(self) -> ExprStmt:
        if self.current().type == ";":
            self.consume(";", "Missing ';'")
            return ExprStmt(None)
        expr = self.parse_expression()
        self.consume(";", "Missing ';'")
        return ExprStmt(expr)

    # expression -> assignment_expression
    def parse_expression(self) -> Node:
        return self.parse_assignment()

    # assignment_expression ::= logical_or_expression [ '=' assignment_expression ]
    def parse_assignment(self) -> Node:
        expr = self.parse_logical_or()
        if self.match("="):
            if not isinstance(expr, Var):
                raise SyntaxError("Invalid assignment target")
            value = self.parse_assignment()
            return Assign(expr.name, value)
        return expr

    # logical_or_expression ::= logical_and_expression { '||' logical_and_expression }
    def parse_logical_or(self) -> Node:
        expr = self.parse_logical_and()
        while self.match("||"):
            right = self.parse_logical_and()
            expr = BinOp("||", expr, right)
        return expr

    # logical_and_expression ::= equality_expression { '&&' equality_expression }
    def parse_logical_and(self) -> Node:
        expr = self.parse_equality()
        while self.match("&&"):
            right = self.parse_equality()
            expr = BinOp("&&", expr, right)
        return expr

    # equality_expression ::= relational_expression { ('==' | '!=') relational_expression }
    def parse_equality(self) -> Node:
        expr = self.parse_relational()
        while self.current().type in {"==", "!="}:
            op = self.current().type
            self.pos += 1
            right = self.parse_relational()
            expr = BinOp(op, expr, right)
        return expr

    # relational_expression ::= additive_expression { ('<=' | '<' | '>' | '>=') additive_expression }
    def parse_relational(self) -> Node:
        expr = self.parse_additive()
        while self.current().type in {"<=", "<", ">", ">="}:
            op = self.current().type
            self.pos += 1
            right = self.parse_additive()
            expr = BinOp(op, expr, right)
        return expr

    # additive_expression ::= term { ('+' | '-') term }
    def parse_additive(self) -> Node:
        expr = self.parse_term()
        while self.current().type in {"+", "-"}:
            op = self.current().type
            self.pos += 1
            right = self.parse_term()
            expr = BinOp(op, expr, right)
        return expr

    # term ::= factor { ('*' | '/') factor }
    def parse_term(self) -> Node:
        expr = self.parse_factor()
        while self.current().type in {"*", "/"}:
            op = self.current().type
            self.pos += 1
            right = self.parse_factor()
            expr = BinOp(op, expr, right)
        return expr

    # factor ::= '(' expression ')' | identifier | call | NUMBER
    def parse_factor(self) -> Node:
        tok = self.current()
        if tok.type == "(":
            self.consume("(", "Expected '('")
            expr = self.parse_expression()
            self.consume(")", "Expected ')'")
            return expr
        if tok.type == "NUMBER":
            self.pos += 1
            return Num(int(tok.value))
        if tok.type == "ID":
            name = tok.value
            self.pos += 1
            if self.match("("):
                args: List[Node] = []
                if self.current().type != ")":
                    args.append(self.parse_expression())
                    while self.match(","):
                        args.append(self.parse_expression())
                self.consume(")", "Expected ')'")
                return Call(name, args)
            return Var(name)
        raise SyntaxError("Unexpected token in expression")

# ---------------------------------------------------------------------------
# Bytecode builder
# ---------------------------------------------------------------------------

class BytecodeBuilder:
    def __init__(self, opcodes: Dict[str, int]):
        self.opcodes = opcodes
        self.code: List[int] = []
        self.lines: List[int] = []
        self.constants: List[Tuple[int, Any]] = []
        self.const_map: Dict[Tuple[int, Any], int] = {}

    def emit(self, byte: int) -> None:
        self.code.append(byte & 0xFF)
        self.lines.append(0)

    def emit_short(self, value: int) -> None:
        self.emit((value >> 8) & 0xFF)
        self.emit(value & 0xFF)

    def add_constant(self, ctype: int, value: Any) -> int:
        key = (ctype, value)
        if key in self.const_map:
            return self.const_map[key]
        idx = len(self.constants)
        self.constants.append((ctype, value))
        self.const_map[key] = idx
        return idx

    def emit_constant(self, value: int) -> None:
        idx = self.add_constant(TYPE_INTEGER, value)
        if idx <= 0xFF:
            self.emit(self.opcodes["OP_CONSTANT"])
            self.emit(idx)
        else:
            self.emit(self.opcodes["OP_CONSTANT16"])
            self.emit_short(idx)

    def patch_short(self, pos: int, value: int) -> None:
        self.code[pos] = (value >> 8) & 0xFF
        self.code[pos + 1] = value & 0xFF

# ---------------------------------------------------------------------------
# Compiler
# ---------------------------------------------------------------------------

class TinyCCompiler:
    def __init__(self, root: Path):
        self.root = root
        self.opcodes = load_opcodes(root)
        self.builder = BytecodeBuilder(self.opcodes)
        self.var_name_idx: Dict[str, int] = {}
        self.func_name_idx: Dict[str, int] = {}
        self.functions: Dict[str, Tuple[int, int]] = {}  # name -> (address, locals)
        self.call_patches: List[Tuple[str, int]] = []
        self.type_integer_idx = self.builder.add_constant(TYPE_STRING, "integer")

    # --- top-level compile ---
    def compile(self, program: Program) -> BytecodeBuilder:
        globals: List[str] = []
        funs: List[FunDecl] = []
        for decl in program.decls:
            if isinstance(decl, VarDecl):
                globals.append(decl.name)
            elif isinstance(decl, FunDecl):
                funs.append(decl)
        # emit global variable definitions
        for name in globals:
            name_idx = self.builder.add_constant(TYPE_STRING, name)
            self.var_name_idx[name] = name_idx
            if name_idx <= 0xFF:
                self.builder.emit(self.opcodes["OP_DEFINE_GLOBAL"])
                self.builder.emit(name_idx)
            else:
                self.builder.emit(self.opcodes["OP_DEFINE_GLOBAL16"])
                self.builder.emit_short(name_idx)
            self.builder.emit(TYPE_INTEGER)
            self.builder.emit_short(self.type_integer_idx)
        # entry stub: call main then halt
        main_name_idx = self.builder.add_constant(TYPE_STRING, "main")
        self.func_name_idx["main"] = main_name_idx
        self.builder.emit(self.opcodes["OP_CALL"])
        self.builder.emit_short(main_name_idx)
        main_addr_patch = len(self.builder.code)
        self.builder.emit_short(0)
        self.builder.emit(0)  # argc
        self.builder.emit(self.opcodes["OP_HALT"])
        self.call_patches.append(("main", main_addr_patch))
        # compile functions
        for f in funs:
            self.compile_function(f)
        # patch call addresses
        for name, pos in self.call_patches:
            if name not in self.functions:
                raise ValueError(f"Undefined function {name}")
            addr, _ = self.functions[name]
            self.builder.patch_short(pos, addr)
        return self.builder

    # --- function compilation ---
    def compile_function(self, f: FunDecl) -> None:
        name_idx = self.builder.add_constant(TYPE_STRING, f.name)
        self.func_name_idx[f.name] = name_idx
        start_addr = len(self.builder.code)
        # parameter and local variable management
        locals: Dict[str, int] = {}
        slot = 0
        for p in f.params:
            locals[p.name] = slot
            slot += 1
        for v in f.body.vars:
            locals[v.name] = slot
            # initialize to 0
            self.builder.emit_constant(0)
            self.builder.emit(self.opcodes["OP_SET_LOCAL"])
            self.builder.emit(slot)
            slot += 1
        for stmt in f.body.statements:
            self.compile_statement(stmt, locals)
        # ensure function returns
        self.builder.emit(self.opcodes["OP_RETURN"])
        self.functions[f.name] = (start_addr, slot)

    # --- statement compilation ---
    def compile_statement(self, stmt: Node, locals: Dict[str, int]) -> None:
        if isinstance(stmt, CompoundStmt):
            new_locals = locals.copy()
            slot = max(new_locals.values(), default=-1) + 1
            for v in stmt.vars:
                new_locals[v.name] = slot
                self.builder.emit_constant(0)
                self.builder.emit(self.opcodes["OP_SET_LOCAL"])
                self.builder.emit(slot)
                slot += 1
            for s in stmt.statements:
                self.compile_statement(s, new_locals)
        elif isinstance(stmt, ExprStmt):
            if stmt.expr:
                self.compile_expression(stmt.expr, locals, expect_result=False)
        elif isinstance(stmt, IfStmt):
            self.compile_expression(stmt.cond, locals, expect_result=True)
            self.builder.emit(self.opcodes["OP_JUMP_IF_FALSE"])
            patch_else = len(self.builder.code)
            self.builder.emit_short(0)
            self.compile_statement(stmt.then_branch, locals)
            if stmt.else_branch:
                self.builder.emit(self.opcodes["OP_JUMP"])
                patch_end = len(self.builder.code)
                self.builder.emit_short(0)
                offset = len(self.builder.code) - (patch_else + 2)
                self.builder.patch_short(patch_else, offset)
                self.compile_statement(stmt.else_branch, locals)
                offset = len(self.builder.code) - (patch_end + 2)
                self.builder.patch_short(patch_end, offset)
            else:
                offset = len(self.builder.code) - (patch_else + 2)
                self.builder.patch_short(patch_else, offset)
        elif isinstance(stmt, WhileStmt):
            loop_start = len(self.builder.code)
            self.compile_expression(stmt.cond, locals, expect_result=True)
            self.builder.emit(self.opcodes["OP_JUMP_IF_FALSE"])
            patch_exit = len(self.builder.code)
            self.builder.emit_short(0)
            self.compile_statement(stmt.body, locals)
            self.builder.emit(self.opcodes["OP_JUMP"])
            backward = loop_start - (len(self.builder.code) + 2)
            self.builder.emit_short(backward)
            offset = len(self.builder.code) - (patch_exit + 2)
            self.builder.patch_short(patch_exit, offset)
        elif isinstance(stmt, ReturnStmt):
            if stmt.expr:
                self.compile_expression(stmt.expr, locals, expect_result=True)
            self.builder.emit(self.opcodes["OP_RETURN"])
        else:
            raise NotImplementedError(f"Unhandled statement {stmt}")

    # --- expression compilation ---
    def compile_expression(self, expr: Node, locals: Dict[str, int], expect_result: bool) -> None:
        if isinstance(expr, Num):
            self.builder.emit_constant(expr.value)
        elif isinstance(expr, Var):
            if expr.name in locals:
                self.builder.emit(self.opcodes["OP_GET_LOCAL"])
                self.builder.emit(locals[expr.name])
            else:
                name_idx = self.var_name_idx[expr.name]
                if name_idx <= 0xFF:
                    self.builder.emit(self.opcodes["OP_GET_GLOBAL"])
                    self.builder.emit(name_idx)
                else:
                    self.builder.emit(self.opcodes["OP_GET_GLOBAL16"])
                    self.builder.emit_short(name_idx)
        elif isinstance(expr, Assign):
            self.compile_expression(expr.expr, locals, expect_result=True)
            if expr.name in locals:
                self.builder.emit(self.opcodes["OP_SET_LOCAL"])
                self.builder.emit(locals[expr.name])
            else:
                name_idx = self.var_name_idx[expr.name]
                if name_idx <= 0xFF:
                    self.builder.emit(self.opcodes["OP_SET_GLOBAL"])
                    self.builder.emit(name_idx)
                else:
                    self.builder.emit(self.opcodes["OP_SET_GLOBAL16"])
                    self.builder.emit_short(name_idx)
            if not expect_result:
                self.builder.emit(self.opcodes["OP_POP"])
        elif isinstance(expr, BinOp):
            self.compile_expression(expr.left, locals, expect_result=True)
            self.compile_expression(expr.right, locals, expect_result=True)
            opmap = {
                "+": "OP_ADD",
                "-": "OP_SUBTRACT",
                "*": "OP_MULTIPLY",
                "/": "OP_DIVIDE",
                "<": "OP_LESS",
                "<=": "OP_LESS_EQUAL",
                ">": "OP_GREATER",
                ">=": "OP_GREATER_EQUAL",
                "==": "OP_EQUAL",
                "!=": "OP_NOT_EQUAL",
                "&&": "OP_AND",
                "||": "OP_OR",
            }
            self.builder.emit(self.opcodes[opmap[expr.op]])
        elif isinstance(expr, Call):
            for arg in expr.args:
                self.compile_expression(arg, locals, expect_result=True)
            if expr.name in self.functions:
                name_idx = self.builder.add_constant(TYPE_STRING, expr.name)
                self.builder.emit(self.opcodes["OP_CALL"])
                self.builder.emit_short(name_idx)
                patch_pos = len(self.builder.code)
                self.builder.emit_short(0)
                self.builder.emit(len(expr.args))
                self.call_patches.append((expr.name, patch_pos))
            else:
                name_idx = self.builder.add_constant(TYPE_STRING, expr.name)
                if expect_result:
                    self.builder.emit(self.opcodes["OP_CALL_BUILTIN"])
                else:
                    self.builder.emit(self.opcodes["OP_CALL_BUILTIN_PROC"])
                self.builder.emit_short(name_idx)
                self.builder.emit(len(expr.args))
            if not expect_result:
                # discard return value when used as statement
                if expr.name in self.functions:
                    self.builder.emit(self.opcodes["OP_POP"])
        else:
            raise NotImplementedError(f"Unhandled expression {expr}")

# ---------------------------------------------------------------------------
# Serialization
# ---------------------------------------------------------------------------

CACHE_MAGIC = 0x50534243  # 'PSBC'
CACHE_VERSION = 4


def write_bytecode(builder: BytecodeBuilder, path: Path, procedures: Dict[str, Tuple[int, int]]) -> None:
    code = bytes(builder.code)
    lines = [0] * len(code)
    consts = builder.constants
    with open(path, "wb") as f:
        f.write(struct.pack("<II", CACHE_MAGIC, CACHE_VERSION))
        f.write(struct.pack("<ii", len(code), len(consts)))
        f.write(code)
        f.write(struct.pack("<" + "i" * len(lines), *lines))
        for ctype, val in consts:
            f.write(struct.pack("<i", ctype))
            if ctype == TYPE_INTEGER:
                f.write(struct.pack("<q", int(val)))
            elif ctype == TYPE_STRING:
                if val is None:
                    f.write(struct.pack("<i", -1))
                else:
                    data = val.encode("utf-8")
                    f.write(struct.pack("<i", len(data)))
                    if data:
                        f.write(data)
            else:
                raise NotImplementedError("Unsupported constant type")
        # Procedure table
        f.write(struct.pack("<i", len(procedures)))
        for name, (addr, locals_count) in sorted(procedures.items()):
            data = name.encode("utf-8")
            f.write(struct.pack("<i", len(data)))
            f.write(data)
            f.write(struct.pack("<i", addr))
            f.write(struct.pack("<i", locals_count))
            f.write(struct.pack("<i", 0))  # upvalue count
            f.write(struct.pack("<i", 0))  # type
        # Const symbol and type tables (empty)
        f.write(struct.pack("<i", 0))
        f.write(struct.pack("<i", 0))

# ---------------------------------------------------------------------------
# Main entry
# ---------------------------------------------------------------------------

def main(argv: List[str]) -> int:
    if len(argv) != 3:
        print("Usage: tinycc.py source.c output.pbc", file=sys.stderr)
        return 1
    src_path = Path(argv[1])
    out_path = Path(argv[2])
    source = src_path.read_text(encoding="utf-8")
    tokens = tokenize(source)
    program = Parser(tokens).parse_program()
    compiler = TinyCCompiler(Path(__file__).resolve().parent.parent)
    builder = compiler.compile(program)
    write_bytecode(builder, out_path, compiler.functions)
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
