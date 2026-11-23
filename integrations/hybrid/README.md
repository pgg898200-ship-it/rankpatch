# Live hybrid document search

Use `CatalogFusion` to combine revised search results by string ID. Give it one document catalog per query. Each worker replaces its own ranked list, and each winning hit carries its original document and fused score.

Install the adapter from this checkout. It uses only RankPatch and the Python standard library.

```sh
python -m pip install .
```

The published v0.1.0 wheels predate this module. For projects pinned to that release, copy [catalog.py](../../python/rankpatch/catalog.py) next to your application and import `CatalogFusion` from `catalog`.

```python
from rankpatch.catalog import CatalogFusion

fusion = CatalogFusion(
    sources=["keyword", "vector", "reranker"],
    documents={"a": {"title": "Caching"}, "b": {"title": "Decoding"}},
    rank_points=[5, 2], k=2,
)
fusion.replace("keyword", 0, ["a", "b"])
fusion.replace("vector", 0, ["b", "a"])
fusion.replace("reranker", 3, ["b"])
assert [(hit.id, hit.score) for hit in fusion.top()] == [("b", 12), ("a", 7)]
assert fusion.top()[0].document["title"] == "Decoding"
fusion.replace("reranker", 4, [])  # remove this source's previous contribution
```

For retrievers that frequently withdraw tied leaders, pass `strategy="steady"` to
`CatalogFusion`. The default is `"buffered"`. Both return the same ordered hits.
In the [65,536-source C++ stress test](../../bench/stress/README.md), steady reduced
p99 on tied withdrawals, while buffered had lower p99 on sparse replacements.
These timings cover the native update and top-ten read. Measure your own stream
including ID mapping, Python hit construction and retrieval.

## SQLite and scikit-learn

The [five-document example](example.py) uses SQLite FTS5 and scikit-learn nearest neighbors. Run it from a checkout with SQLite FTS5 enabled.

```sh
python -m pip install scikit-learn==1.8.0
python integrations/hybrid/example.py
python -m unittest discover -s integrations/hybrid -p test_hybrid.py
```

For SQLite, set `connection.row_factory = sqlite3.Row` and pass an ordered cursor to `replace_sqlite(source, revision, cursor, id_column="doc_id")`. For scikit-learn, pass one query's neighbor indices to `replace_neighbors(source, revision, indices, fit_ids=fit_ids)`. Preserve the IDs in training-matrix row order in `fit_ids`, including when rebuilding the neighbor index.

The catalog and source names are fixed at construction. Use one fusion index per active query and serialize updates for that query. Equal scores follow ascending Python string order, independent of arrival order. Unknown document IDs reject a fresh snapshot and preserve the previous revision. A retraction removes a source's votes while keeping its revision watermark. The caller owns document contents, and mutations to returned documents are visible through the catalog.

Each instance builds a dense ID map over its entire supplied catalog. For a large index, the caller can supply a bounded per-session candidate catalog if it covers all possible revisions. RankPatch measures the cost of maintaining the fused result after a retrieval update. SQLite query execution, vector search, and Python document materialization add separate costs.

## LangChain retrievers

A LangChain retriever returns a ranked list of `Document` objects. Give each chunk a stable string ID shared across retrievers. Pass the IDs in result order to RankPatch after each worker finishes. Read the winners before building the model’s context. Each hit retains its original catalog document, including page content and metadata.

The [runnable example](langchain_example.py) uses a local LangChain `InMemoryVectorStore` and TF-IDF vectors. It expands one search from one hit to two, applies an editorial ranking, withdraws it and rejects a replay. Run it from a checkout with RankPatch installed.

```sh
python -m pip install langchain-core==1.8.4 scikit-learn==1.8.0
python integrations/hybrid/langchain_example.py
```

```text
prefix: reuse cached prefix tokens
evict: evict unused cached blocks
```

For an existing retriever, the handoff below assumes one coordinator owns the `fusion` instance and its catalog contains every possible result ID. Set `Document.id` when indexing chunks, or use your canonical chunk-ID metadata field in its place.

```python
results = retriever.invoke(query)
fusion.replace("search", revision, (doc.id for doc in results))
context_docs = [hit.document for hit in fusion.top()]
```

[LangChain retriever interface](https://docs.langchain.com/oss/python/integrations/retrievers)
