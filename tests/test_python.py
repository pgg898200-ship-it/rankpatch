"""Run after pip install . with: python -m unittest discover -s tests -p test_python.py"""
import unittest

from rankpatch import Index


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
