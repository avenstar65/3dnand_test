#!/usr/bin/env python3
"""Architecture guards for the split QEMU 3D NAND Linux driver."""

import pathlib
import re
import tempfile
import unittest


CONTROL_WORDS = {"if", "for", "while", "switch"}


def strip_comments_and_literals(source):
    """Replace comments and literals with spaces while preserving newlines."""
    out = list(source)
    index = 0
    state = "code"
    while index < len(source):
        char = source[index]
        nxt = source[index + 1] if index + 1 < len(source) else ""
        if state == "code" and char == "/" and nxt == "*":
            out[index:index + 2] = "  "
            state, index = "block", index + 2
        elif state == "code" and char == "/" and nxt == "/":
            out[index:index + 2] = "  "
            state, index = "line", index + 2
        elif state == "code" and char in {'"', "'"}:
            out[index] = " "
            state, index = ("string" if char == '"' else "char"), index + 1
        elif state == "block" and char == "*" and nxt == "/":
            out[index:index + 2] = "  "
            state, index = "code", index + 2
        elif state == "line" and char == "\n":
            state, index = "code", index + 1
        elif state in {"string", "char"} and char == "\\":
            out[index] = " "
            if index + 1 < len(source) and source[index + 1] != "\n":
                out[index + 1] = " "
            index += 2
        elif ((state == "string" and char == '"') or
              (state == "char" and char == "'")):
            out[index] = " "
            state, index = "code", index + 1
        else:
            if state != "code" and char != "\n":
                out[index] = " "
            index += 1
    return "".join(out)


def _effective_lines(source, start, end):
    body = source[start + 1:end]
    return sum(bool(line.strip()) for line in body.splitlines())


def function_spans(source):
    clean = strip_comments_and_literals(source)
    spans = []
    depth = 0
    body_start = None
    name = None
    for index, char in enumerate(clean):
        if char == "{" and depth == 0:
            prefix = clean[:index].rstrip()
            match = re.search(r"([A-Za-z_]\w*)\s*\([^;{}]*\)\s*$", prefix)
            candidate = match.group(1) if match else None
            line_start = prefix.rfind("\n") + 1
            if (candidate and candidate not in CONTROL_WORDS and
                    not prefix[line_start:].lstrip().startswith("#")):
                name, body_start = candidate, index
            depth += 1
        elif char == "{" and depth:
            depth += 1
        elif char == "}" and depth:
            depth -= 1
            if depth == 0 and body_start is not None:
                start_line = clean.count("\n", 0, body_start) + 1
                end_line = clean.count("\n", 0, index) + 1
                effective = _effective_lines(clean, body_start, index)
                spans.append((name, start_line, end_line, effective))
                name, body_start = None, None
    return spans


def find_oversized_functions(source, limit):
    return [span for span in function_spans(source) if span[3] > limit]


def dependency_cycles(graph):
    visiting, visited, cycles = set(), set(), []

    def visit(node, path):
        if node in visiting:
            cycles.append(path[path.index(node):] + [node])
            return
        if node in visited:
            return
        visiting.add(node)
        for target in graph.get(node, set()):
            if target in graph:
                visit(target, path + [target])
        visiting.remove(node)
        visited.add(node)

    for node in graph:
        visit(node, [node])
    return cycles


class FunctionScannerTests(unittest.TestCase):
    def test_function_limit_accepts_50_effective_lines(self):
        body = "\n".join(f"\tv += {i};" for i in range(49))
        source = f"static int q3n_ok(int v)\n{{\n{body}\n\treturn v;\n}}\n"
        self.assertEqual(find_oversized_functions(source, 50), [])

    def test_function_limit_rejects_51_effective_lines(self):
        body = "\n".join(f"\tv += {i};" for i in range(50))
        source = f"static int q3n_too_long(int v)\n{{\n{body}\n\treturn v;\n}}\n"
        self.assertEqual(find_oversized_functions(source, 50)[0][0],
                         "q3n_too_long")

    def test_comments_literals_and_multiline_signature_are_ignored(self):
        source = '''
static int
q3n_fixture(int value,
            int other)
{
    /* { ignored } */
    const char *text = "}";
    return value + other; // {
}
'''
        self.assertEqual(function_spans(source)[0][0], "q3n_fixture")
        self.assertEqual(function_spans(source)[0][3], 2)


class ArchitectureHelperTests(unittest.TestCase):
    def test_dependency_cycle_detection(self):
        self.assertEqual(dependency_cycles({"a": {"b"}, "b": set()}), [])
        self.assertTrue(dependency_cycles({"a": {"b"}, "b": {"a"}}))

    def test_file_ownership_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "owner.c").write_text("void q3n_owned(void) {}\n")
            (root / "other.c").write_text("void unrelated(void) {}\n")
            self.assertIn("q3n_owned", (root / "owner.c").read_text())
            self.assertNotIn("q3n_owned", (root / "other.c").read_text())


if __name__ == "__main__":
    unittest.main()
