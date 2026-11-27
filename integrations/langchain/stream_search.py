"""Stream ranked documents to a RAG client as LangChain retrievers finish."""
import asyncio
from dataclasses import dataclass
from itertools import islice

from langchain_core.documents import Document
from langchain_core.runnables import RunnableLambda, RunnableParallel
from rank_bm25 import BM25L

from rankpatch.catalog import CatalogFusion


@dataclass
class Revision:
    version: int
    documents: list[Document]


def _parent_ids(documents, key):
    seen = set()
    for doc in documents:
        parent = doc.metadata[key]
        if parent not in seen:
            seen.add(parent)
            yield parent


async def stream_top(query, sources, catalog, rank_points, k, *, parent_id_key=None):
    """Yield one snapshot per accepted, complete source revision for this query."""
    if parent_id_key is not None:
        rank_points = tuple(rank_points)
    fusion = CatalogFusion(sources=sources, documents=catalog, rank_points=rank_points, k=k)
    async for chunk in RunnableParallel(sources).astream(query):
        for source, event in chunk.items():
            if parent_id_key is None:
                ids = (doc.id for doc in event.documents)
            else:
                ids = islice(_parent_ids(event.documents, parent_id_key), len(rank_points))
            if fusion.replace(source, event.version, ids):
                yield source, event.version, fusion.top()


def bm25_search(documents, field):
    """Build a local BM25 runnable over one document field."""
    docs = list(documents)
    model = BM25L([field(doc).casefold().split() for doc in docs])

    def search(query):
        scores = model.get_scores(query.casefold().split())
        matches = [(score, doc) for score, doc in zip(scores, docs) if score > 0]
        matches.sort(key=lambda item: (-item[0], item[1].id))
        return [doc for _, doc in matches[:2]]

    return RunnableLambda(search)


def build_example():
    rows = [
        ("eviction", "Cache eviction", "Eviction happens when the prefix cache fills."),
        ("routing", "Prefix cache routing", "Route warm requests to the same worker."),
        ("shared", "Shared prefixes", "A prefix cache lets requests reuse earlier tokens."),
        ("batching", "Batch size", "Group work to keep the device busy."),
        ("memory", "Device memory", "Reserve memory for live requests."),
        ("queue", "Queue depth", "Track waiting requests separately."),
    ]
    catalog = {
        key: Document(id=key, page_content=body, metadata={"title": title})
        for key, title, body in rows
    }
    titles = bm25_search(catalog.values(), lambda doc: doc.metadata["title"])
    bodies = bm25_search(catalog.values(), lambda doc: doc.page_content)

    async def title_updates(query):
        candidates = await titles.ainvoke(query)
        yield Revision(0, candidates)
        # Promote exact phrase matches in the body, retaining title order on ties.
        phrase = query.casefold()
        reranked = sorted(candidates, key=lambda d: phrase not in catalog[d.id].page_content.casefold())
        yield Revision(1, reranked)

    sources = {
        "body": bodies | RunnableLambda(lambda docs: Revision(0, docs)),
        "title": RunnableLambda(title_updates),
    }
    return catalog, sources


async def main():
    catalog, sources = build_example()
    async for source, version, hits in stream_top("prefix cache", sources, catalog, [3, 1], 3):
        ranking = ", ".join(f"{h.document.metadata['title']} ({h.score})" for h in hits)
        print(f"{source} revision {version} -> {ranking}")


if __name__ == "__main__":
    asyncio.run(main())
