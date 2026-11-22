# Live hybrid document search

Use `CatalogFusion` when a keyword retriever, nearest-neighbor index and reranker can send revised ranked lists for the same query. It accepts string document IDs, translates them to the dense integers used by RankPatch, and returns the original document objects with their fused scores. The example executes real SQLite FTS5 queries and real scikit-learn neighbor searches over five documents.

From the repository root, run

```sh
python -m pip install . scikit-learn==1.8.0
python integrations/hybrid/example.py
python -m unittest discover -s integrations/hybrid -p test_hybrid.py
```

Use a Python installation with SQLite FTS5 enabled. The example and tests ran with CPython 3.12.14, SQLite 3.53.1 and scikit-learn 1.8.0. The adapter itself imports only RankPatch and the Python standard library. Copy `catalog.py` into a consumer project or import it from a checkout.

```python
from integrations.hybrid.catalog import CatalogFusion

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

For SQLite, set `connection.row_factory = sqlite3.Row` and pass an ordered cursor to `replace_sqlite(source, revision, cursor, id_column="doc_id")`. For scikit-learn, pass one query's neighbor indices to `replace_neighbors(source, revision, indices, fit_ids=fit_ids)`. Preserve the IDs in training-matrix row order in `fit_ids`, including when rebuilding the neighbor index.

The catalog and source names are fixed at construction. Use one fusion index per active query and serialize updates for that query. Equal scores follow ascending Python string order, independent of arrival order. Unknown document IDs reject a fresh snapshot and preserve the previous revision. A retraction removes a source's votes while keeping its revision watermark. The caller owns document contents, and mutations to returned documents are visible through the catalog.

Each instance builds a dense ID map over its entire supplied catalog. For a large index, the caller can supply a bounded per-session candidate catalog if it covers all possible revisions. RankPatch measures the cost of maintaining the fused result after a retrieval update. SQLite query execution, vector search, and Python document materialization add separate costs.
