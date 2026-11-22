import unittest

from catalog import CatalogFusion
from example import DOCUMENTS, connect_fts, run


class HybridTests(unittest.TestCase):
    def test_real_engines(self):
        hits = run()
        self.assertEqual(hits[0].id, "spec/decode")
        self.assertEqual(hits[0].score, 10)
        for h in hits:
            self.assertIs(h.document, DOCUMENTS[h.id])

    def test_fts_retraction_and_replay(self):
        fusion = CatalogFusion(sources=["fts", "other"], documents=DOCUMENTS,
                               rank_points=[5, 3, 1], k=3)
        db = connect_fts()
        try:
            rows = db.execute("SELECT doc_id FROM docs WHERE docs MATCH ? ORDER BY rank, doc_id",
                              ("tokens",))
            self.assertTrue(fusion.replace_sqlite("fts", 0, rows))
            self.assertEqual({hit.id for hit in fusion.top()}, {"spec/cache", "spec/stream"})
            self.assertFalse(fusion.replace("fts", 0, ["unknown"]))
            self.assertTrue(fusion.replace("fts", 1, []))
            self.assertEqual(fusion.top(), [])
        finally:
            db.close()

    def test_mapping_failure_preserves_revision(self):
        fusion = CatalogFusion(sources=("a", "b"), documents=DOCUMENTS,
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


if __name__ == "__main__":
    unittest.main()
