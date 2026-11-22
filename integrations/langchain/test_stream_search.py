import unittest

from langchain_core.runnables import RunnableLambda

from integrations.langchain.stream_search import Revision, build_example, stream_top


class StreamTests(unittest.IsolatedAsyncioTestCase):
    async def test_real_bm25_retrievers(self):
        catalog, sources = build_example()
        ballots = {
            ("body", 0): ["eviction", "shared"],
            ("title", 0): ["routing", "eviction"],
            ("title", 1): ["eviction", "routing"],
        }
        latest = {}
        events = []
        async for source, version, hits in stream_top("prefix cache", sources, catalog, [3, 1], 3):
            events.append((source, version))
            latest[source] = ballots[source, version]
            scores = {}
            for ballot in latest.values():
                for doc_id, points in zip(ballot, [3, 1]):
                    scores[doc_id] = scores.get(doc_id, 0) + points
            expected = sorted(scores.items(), key=lambda hit: (-hit[1], hit[0]))[:3]
            self.assertEqual([(h.id, h.score) for h in hits], expected)
            for hit in hits:
                self.assertIs(hit.document, catalog[hit.id])
        self.assertCountEqual(events, ballots.keys())

    async def test_retry_and_withdrawal(self):
        catalog, _ = build_example()

        async def revised(query):
            yield Revision(1, [catalog["shared"]])
            yield Revision(0, [catalog["routing"]])
            yield Revision(1, [catalog["eviction"]])
            yield Revision(2, [])

        sources = {"search": RunnableLambda(revised)}
        snapshots = [(v, [h.id for h in hits]) async for _, v, hits in
                     stream_top("prefix cache", sources, catalog, [3, 1], 3)]
        self.assertEqual(snapshots, [(1, ["shared"]), (2, [])])
