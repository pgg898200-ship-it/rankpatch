"""Run after pip install . with: python -m unittest discover -s tests -p test_python.py"""
from collections import Counter
import unittest

from rankpatch import Index
from rankpatch.catalog import CatalogFusion, DocumentCatalog


class BindingContract:
    def make_index(self, *args, **kwargs):
        return Index(*args, **kwargs, strategy=self.strategy)

    def test_revisions_and_snapshot(self):
        idx = self.make_index(sources=2, universe=8, rank_points=[4, 2], k=3)
        self.assertEqual(idx.top(), [])
        self.assertTrue(idx.replace(source=0, revision=0, ids=iter([3, 2])))
        first = idx.top()
        self.assertTrue(idx.replace(1, 1, [2, 5]))
        self.assertFalse(idx.replace(1, 0, [6]))
        self.assertFalse(idx.replace(1, 1, []))
        self.assertFalse(idx.replace(1, 0, None))
        self.assertEqual(idx.top(), [(2, 6), (3, 4), (5, 2)])
        self.assertTrue(idx.replace(0, 2, []))
        self.assertEqual(idx.top(), [(2, 4), (5, 2)])
        self.assertEqual(first, [(3, 4), (2, 2)])

    def test_numeric_boundaries(self):
        idx = self.make_index(2, 3, [2**32 - 1], 1)
        for s in range(2):
            self.assertTrue(idx.replace(s, 2**64 - 1, [2]))
        self.assertEqual(idx.top(), [(2, 2 * (2**32 - 1))])
        for revision in (-1, 2**64, 1.0, True):
            with self.assertRaises((TypeError, OverflowError)):
                idx.replace(1, revision, [])
        for points in ([-1], [2**32], [False], [1.5]):
            with self.assertRaises((TypeError, OverflowError)):
                self.make_index(1, 3, points, 1)

    def test_invalid_update_is_atomic(self):
        idx = self.make_index(1, 4, [5, 1], 2)
        idx.replace(0, 1, [0])
        for ids in ([2, 2], [4], [2**32], [-1], [True], [0, 1, 2]):
            with self.assertRaises((IndexError, ValueError, TypeError, OverflowError)):
                idx.replace(0, 2, ids)
            self.assertEqual(idx.top(), [(0, 5)])
        self.assertTrue(idx.replace(0, 2, [3]))
        self.assertEqual(idx.top(), [(3, 5)])


class BufferedTests(BindingContract, unittest.TestCase):
    strategy = "buffered"


class SteadyTests(BindingContract, unittest.TestCase):
    strategy = "steady"


class CatalogTests(unittest.TestCase):
    def test_queries_share_document_references_and_keep_separate_revisions(self):
        original = object()
        docs = {"b": object(), "a": original}
        catalog = DocumentCatalog(docs)
        docs.clear()
        first, second = [
            CatalogFusion(sources=["search"], documents=catalog, rank_points=[1, 1],
                          k=2, strategy=strategy)
            for strategy in ("buffered", "steady")
        ]
        self.assertTrue(first.replace("search", 9, ["b", "a"]))
        self.assertEqual(second.top(), [])
        self.assertTrue(second.replace("search", 0, ["a"]))
        self.assertTrue(second.replace("search", 1, []))
        self.assertEqual([hit.id for hit in first.top()], ["a", "b"])
        self.assertIs(first.top()[0].document, original)

    def test_withdrawing_tied_leaders(self):
        keys = [f"doc-{i:02}" for i in range(32)]
        docs = {key: object() for key in reversed(keys)}
        names = [f"worker-{i}" for i in range(16)]
        for strategy in ("buffered", "steady"):
            with self.subTest(strategy=strategy):
                fusion = CatalogFusion(sources=names, documents=docs,
                                       rank_points=[1, 1], k=3, strategy=strategy)
                ballots = {}
                for revision in (0, 1):
                    for i, name in enumerate(names):
                        ids = keys[2*i:2*i+2] if revision == 0 else []
                        self.assertTrue(fusion.replace(name, revision, ids))
                        ballots[name] = ids
                        scores = Counter(key for ballot in ballots.values() for key in ballot)
                        expected = sorted(scores.items(), key=lambda row: (-row[1], row[0]))[:3]
                        self.assertEqual([(h.id, h.score) for h in fusion.top()], expected)

    def test_unknown_strategy_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "strategy"):
            CatalogFusion(sources=["a"], documents={"doc": None},
                          rank_points=[1], k=1, strategy="unknown")

    def test_mapping_failure_preserves_revision(self):
        documents = {key: object() for key in ("spec/cache", "spec/decode", "spec/ledger", "spec/shard")}
        fusion = CatalogFusion(sources=("a", "b"), documents=documents,
                               rank_points=[5, 3], k=3)
        fusion.replace("a", 1, ["spec/cache"])
        for bad in (["spec/ledger", "unknown"], ["spec/decode", "spec/decode"]):
            with self.assertRaises((KeyError, ValueError)):
                fusion.replace("a", 2, bad)
        with self.assertRaises(IndexError):
            fusion.replace_neighbors("a", 2, [-1], fit_ids=["spec/ledger"])
        self.assertEqual([h.id for h in fusion.top()], ["spec/cache"])
        fusion.replace("a", 2, ["spec/shard"])
        fusion.replace("b", 0, ["spec/decode"])
        self.assertEqual([h.id for h in fusion.top()], ["spec/decode", "spec/shard"])


class StrategyTests(unittest.TestCase):
    def test_default(self):
        idx = Index(1, 2, [10], 1)
        self.assertTrue(idx.replace(0, 0, [1]))
        self.assertEqual(idx.top(), [(1, 10)])

    def test_invalid_strategy(self):
        with self.assertRaisesRegex(ValueError, "strategy"):
            Index(1, 2, [10], 1, strategy="slower")
        with self.assertRaises(TypeError):
            Index(1, 2, [10], 1, strategy=None)


if __name__ == "__main__":
    unittest.main()
