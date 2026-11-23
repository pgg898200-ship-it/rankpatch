"""Fuse full source revisions against a fixed string-keyed document catalog."""
from dataclasses import dataclass
from operator import index as integer_index
from typing import Generic, Mapping, TypeVar

from rankpatch import Index

Document = TypeVar("Document")


@dataclass(frozen=True)
class Hit(Generic[Document]):
    id: str
    score: int
    document: Document


class CatalogFusion(Generic[Document]):
    """Owned by one query/coordinator. Documents are returned by reference."""

    def __init__(self, *, sources, documents: Mapping[str, Document], rank_points, k, strategy="buffered"):
        if isinstance(sources, str):
            raise TypeError("sources must be an iterable of names")
        names = tuple(sources)
        if any(not isinstance(name, str) or not name for name in names):
            raise ValueError("source names must be nonempty strings")
        self._sources = {name: i for i, name in enumerate(names)}
        if len(self._sources) != len(names):
            raise ValueError("duplicate source name")

        if any(not isinstance(key, str) for key in documents):
            raise TypeError("document IDs must be strings")
        self._keys = sorted(documents)
        self._rows = {key: i for i, key in enumerate(self._keys)}
        self._documents = [documents[key] for key in self._keys]
        self._index = Index(len(names), len(self._keys), rank_points, k, strategy=strategy)

    def replace(self, source, revision, doc_ids):
        """Apply a complete ordered snapshot, or [] to withdraw the source."""
        # ID lookup happens lazily so stale events skip payload traversal.
        def rows():
            if isinstance(doc_ids, (str, bytes)):
                raise TypeError("doc_ids must be an iterable of string IDs")
            for key in doc_ids:
                yield self._rows[key]
        return self._index.replace(self._sources[source], revision, rows())

    def replace_sqlite(self, source, revision, rows, *, id_column="doc_id"):
        """Accept a cursor/iterable of ordered sqlite3.Row objects."""
        return self.replace(source, revision, (row[id_column] for row in rows))

    def replace_neighbors(self, source, revision, indices, *, fit_ids):
        """Accept one row from sklearn kneighbors(..., return_distance=False).

        fit_ids[i] must identify the document in row i of the fitted matrix.
        """
        def ids():
            for raw in indices:
                row = integer_index(raw)
                if row < 0 or row >= len(fit_ids):
                    raise IndexError(f"neighbor row out of bounds: {row}")
                yield fit_ids[row]
        return self.replace(source, revision, ids())

    def top(self):
        return [Hit(self._keys[i], score, self._documents[i])
                for i, score in self._index.top()]
