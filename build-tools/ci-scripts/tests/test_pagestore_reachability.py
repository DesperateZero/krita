#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regression fixtures for review classifications, not dead-code permission."""
import importlib.util
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location(
    "pagestore_callgraph", Path(__file__).resolve().parents[1] / "generate-pagestore-callgraph.py")
graph = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = graph
spec.loader.exec_module(graph)


def node(name, production=(), tests=(), kind="function"):
    def calls(names, path):
        return [{"caller-usr": n, "caller": n, "path": path, "line": 1} for n in names]
    return {"usr": name, "name": name, "kind": kind,
            "production-callers": calls(production, "libs/image/pagestore/test.cpp"),
            "test-callers": calls(tests, "libs/image/tests/test.cpp"),
            "callback-registrations": [], "specializations-and-overrides": []}


class ReachabilityTest(unittest.TestCase):
    def test_production_file_cycle_is_not_a_root(self):
        review = graph.reachability_review([node("a", ["b"]), node("b", ["a"])])
        self.assertEqual(review["summary"], {"unreached-review": 2})
        self.assertEqual(review["nonproduction-components"], [["a", "b"]])
        self.assertFalse(review["deletion-authorized"])

    def test_tests_do_not_promote_cycle_to_production(self):
        review = graph.reachability_review([node("a", ["b"], ["test"]), node("b", ["a"])])
        self.assertEqual(review["summary"], {"test-only-review": 2})
        self.assertEqual(review["functions"][1]["predecessors"]["test"], "a")

    def test_production_boundary_keeps_transitive_callees(self):
        review = graph.reachability_review([node("a", ["external"]), node("b", ["a"])])
        self.assertEqual(review["summary"], {"production-boundary-reachable": 2})
        self.assertEqual(review["functions"][1]["predecessors"]["production"], "a")

    def test_raii_is_indirect_not_certified_dead(self):
        review = graph.reachability_review([node("dtor", kind="destructor"), node("cancel", ["dtor"])])
        self.assertEqual(review["summary"], {"indirect-review": 2})

    def test_callback_and_override_propagate(self):
        callback = node("callback")
        callback["callback-registrations"] = [{"scope": "production",
            "containers": [{"usr": "entry", "name": "entry"}]}]
        derived = node("derived")
        derived["specializations-and-overrides"] = [{"related-usr": "callback"}]
        review = graph.reachability_review([node("entry", ["external"]), callback, derived])
        self.assertEqual(review["summary"], {"production-boundary-reachable": 3})

    def test_noncall_reference_requires_review(self):
        referenced = node("dependent_member")
        referenced["non-call-references"] = [{"path": "libs/image/pagestore/template.cpp"}]
        review = graph.reachability_review([referenced])
        self.assertEqual(review["summary"], {"indirect-review": 1})


if __name__ == "__main__":
    unittest.main()
