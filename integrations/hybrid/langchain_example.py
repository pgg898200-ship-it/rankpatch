"""Fuse LangChain retrieval snapshots for one query before assembling context."""

from langchain_core.documents import Document
from langchain_core.embeddings import Embeddings
from langchain_core.vectorstores import InMemoryVectorStore
from sklearn.feature_extraction.text import TfidfVectorizer

from catalog import CatalogFusion


class LocalEmbeddings(Embeddings):
    """Use a small TF-IDF vocabulary to keep retrieval reproducible offline."""

    def __init__(self, texts):
        self.vectorizer = TfidfVectorizer().fit(texts)

    def embed_documents(self, texts):
        return self.vectorizer.transform(texts).toarray().tolist()

    def embed_query(self, text):
        return self.embed_documents([text])[0]


def run():
    catalog = {
        "prefix": Document(id="prefix", page_content="reuse cached prefix tokens"),
        "evict": Document(id="evict", page_content="evict unused cached blocks"),
        "trace": Document(id="trace", page_content="trace an inference request"),
    }
    store = InMemoryVectorStore(LocalEmbeddings(doc.page_content for doc in catalog.values()))
    store.add_documents(list(catalog.values()))
    fusion = CatalogFusion(sources=["search", "editor"], documents=catalog,
                           rank_points=[5, 3], k=2)

    # One query coordinator receives complete ranked lists from each source.
    query = "cached prefix tokens"
    for revision, depth in enumerate([1, 2]):
        retriever = store.as_retriever(search_kwargs={"k": depth})
        results = retriever.invoke(query)
        fusion.replace("search", revision, (doc.id for doc in results))

    # A second source promotes the eviction guide, then withdraws it.
    fusion.replace("editor", 0, ["evict"])
    assert [(h.id, h.score) for h in fusion.top()] == [("evict", 8), ("prefix", 5)]
    fusion.replace("editor", 1, [])
    assert not fusion.replace("editor", 0, ["evict"])
    hits = fusion.top()
    assert [(h.id, h.score) for h in hits] == [("prefix", 5), ("evict", 3)]
    return [hit.document for hit in hits]


if __name__ == "__main__":
    for document in run():
        print(f"{document.id}: {document.page_content}")
