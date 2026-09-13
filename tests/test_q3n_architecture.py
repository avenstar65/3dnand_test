#!/usr/bin/env python3
"""Architecture guards for the split QEMU 3D NAND Linux driver."""

import pathlib
import re
import tempfile
import unittest


CONTROL_WORDS = {"if", "for", "while", "switch"}
DRIVER_MODULES = ("main", "device", "hw", "nand", "profile", "serial",
                  "debugfs")
DEPENDENCIES = {
    "main": {"device", "hw", "nand", "debugfs"},
    "nand": {"device", "hw", "profile", "serial"},
    "profile": {"hw"},
    "serial": {"hw"},
    "debugfs": {"hw", "serial"},
    "device": set(),
    "hw": set(),
}


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


def source_dependencies(sources):
    """Build module edges from calls to functions defined by another module."""
    owners = {}
    for module, source in sources.items():
        for name, *_ in function_spans(source):
            owners[name] = module
    graph = {module: set() for module in sources}
    for module, source in sources.items():
        clean = strip_comments_and_literals(source)
        for name, owner in owners.items():
            if owner != module and re.search(rf"\b{re.escape(name)}\s*\(", clean):
                graph[module].add(owner)
    return graph


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

    def test_source_dependencies_follow_real_calls(self):
        sources = {
            "upper": "int lower_api(void); int upper(void) { return lower_api(); }",
            "lower": "int lower_api(void) { return 0; }",
        }
        self.assertEqual(source_dependencies(sources),
                         {"upper": {"lower"}, "lower": set()})


class RepositoryArchitectureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = pathlib.Path(__file__).resolve().parents[1]
        cls.driver = cls.root / "linux/drivers/mtd/nand/raw"

    def source(self, module):
        return (self.driver / f"qemu_3dnand_{module}.c").read_text()

    def test_split_files_and_internal_header_exist(self):
        for module in DRIVER_MODULES:
            self.assertTrue((self.driver / f"qemu_3dnand_{module}.c").is_file())
        self.assertTrue((self.driver / "qemu_3dnand_internal.h").is_file())

    def test_split_functions_have_at_most_50_effective_lines(self):
        failures = []
        for module in DRIVER_MODULES:
            for name, start, end, count in find_oversized_functions(
                    self.source(module), 50):
                failures.append(f"{module}:{start}-{end} {name}={count}")
        self.assertEqual(failures, [])

    def test_responsibility_ownership(self):
        owners = {
            "device": r"(?m)^const struct q3n_device_desc \*q3n_device_match",
            "hw": r"(?m)^int q3n_hw_read_page_locked",
            "nand": r"(?m)^int q3n_nand_register",
            "profile": r"(?m)^int q3n_profile_read_page_locked",
            "serial": r"(?m)^static void qemu_3dnand_parity_worker",
            "debugfs": r"(?m)^void q3n_debugfs_init",
            "main": r"(?m)^static int qemu_3dnand_probe",
        }
        for owner, pattern in owners.items():
            self.assertRegex(self.source(owner), pattern)
            for other in set(DRIVER_MODULES) - {owner}:
                self.assertNotRegex(self.source(other), pattern)

    def test_main_contains_only_lifecycle_functions(self):
        allowed = {
            "qemu_3dnand_free_metadata", "q3n_pci_prepare",
            "q3n_validate_controller", "q3n_read_geometry",
            "q3n_discover_device", "q3n_validate_device_geometry",
            "q3n_configure_geometry",
            "q3n_alloc_buffers", "q3n_alloc_metadata",
            "q3n_alloc_runtime", "q3n_log_geometry", "q3n_probe_failed",
            "qemu_3dnand_probe", "qemu_3dnand_remove",
        }
        found = {span[0] for span in function_spans(self.source("main"))}
        self.assertEqual(found, allowed)

    def test_dependency_graph_is_acyclic(self):
        sources = {module: self.source(module) for module in DRIVER_MODULES}
        graph = source_dependencies(sources)
        unexpected = {module: targets - DEPENDENCIES[module]
                      for module, targets in graph.items()
                      if targets - DEPENDENCIES[module]}
        self.assertEqual(unexpected, {})
        self.assertEqual(dependency_cycles(graph), [])

    def test_device_id_and_build_wiring(self):
        device = self.source("device")
        self.assertIn("0x9c, 0xd7, 0x98, 0xa6, 0x51, 0x33, 0x4e, 0x44",
                      device)
        makefile = (self.driver / "Makefile.qemu_3dnand").read_text()
        overlay = (self.root / "scripts/apply-linux-overlay.sh").read_text()
        for module in DRIVER_MODULES[1:]:
            self.assertIn(f"qemu_3dnand_{module}.o", makefile)
            self.assertIn(f"qemu_3dnand_{module}.c", overlay)


if __name__ == "__main__":
    unittest.main()
