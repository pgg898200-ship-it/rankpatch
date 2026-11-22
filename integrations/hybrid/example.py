"""Run two real retrieval engines and revise their contributions to one query."""
import sqlite3

from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.neighbors import NearestNeighbors

from rankpatch.catalog import CatalogFusion


DOCUMENTS = {
    "spec/cache": {"text": "prefix cache stores tokens for fast inference", "team": "runtime"},
    "spec/shard": {"text": "shard placement balances traffic", "team": "storage"},
    "spec/decode": {"text": "decoding latency can dominate inference", "team": "models"},
    "spec/ledger": {"text": "audit ledger stores revision history", "team": "storage"},
    "spec/stream": {"text": "streaming inference produces tokens", "team": "runtime"},
}


def connect_fts():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE VIRTUAL TABLE docs USING fts5(doc_id UNINDEXED, body)")
    db.executemany("INSERT INTO docs VALUES (?, ?)",
                   ((key, doc["text"]) for key, doc in DOCUMENTS.items()))
    return db


def run():
    fusion = CatalogFusion(sources=["lexical", "neighbors", "reranker"],
                           documents=DOCUMENTS, rank_points=[5, 3, 1], k=3)
    db = connect_fts()
    try:
        rows = db.execute("SELECT doc_id FROM docs WHERE docs MATCH ? ORDER BY rank, doc_id LIMIT 3",
                          ("inference OR tokens",))
        fusion.replace_sqlite("lexical", 1, rows)
    finally:
        db.close()

    # fit_ids preserves matrix row identity independently of the catalog's tie order.
    fit_ids = tuple(DOCUMENTS)
    vectorizer = TfidfVectorizer(analyzer="char_wb", ngram_range=(3, 5))
    matrix = vectorizer.fit_transform([DOCUMENTS[key]["text"] for key in fit_ids])
    neighbors = NearestNeighbors(metric="cosine", algorithm="brute").fit(matrix)
    for revision, query in [(0, "token inference"), (1, "low latency decoding")]:
        rows = neighbors.kneighbors(vectorizer.transform([query]), n_neighbors=3,
                                    return_distance=False)[0]
        fusion.replace_neighbors("neighbors", revision, rows, fit_ids=fit_ids)

    fusion.replace("reranker", 4, ["spec/decode", "spec/cache"])
    assert not fusion.replace("reranker", 3, ["spec/ledger"])
    fusion.replace("lexical", 2, [])
    return fusion.top()


if __name__ == "__main__":
    for hit in run():
        print(f"{hit.score:>2}  {hit.id:<14} {hit.document['team']}")
