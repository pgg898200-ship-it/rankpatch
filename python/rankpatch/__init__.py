"""Maintain an exact top-k across independently revised, bounded ranked lists."""
from . import _native

__all__ = ["Index"]
__version__ = "0.2.0"


class Index:
    """A single in-process index with integer source and document IDs.

    Each live source contributes rank_points[i] to its document at position i.
    IDs range from zero to universe - 1 and each source list must be unique.
    The index uses memory proportional to the configured document universe.
    strategy="buffered" caches top candidates and refills them on demand.
    strategy="steady" maintains ordered winners and a heap of runners-up
    at update time. Both strategies return the same ranked snapshots.
    """

    __slots__ = ("_state",)

    def __init__(self, sources, universe, rank_points, k, *, strategy="buffered"):
        self._state = _native.create(sources, universe, rank_points, k, strategy)

    def replace(self, source, revision, ids):
        """Replace the entire source list. Pass [] to retract it.

        Return True when applied. An equal or older revision returns False.
        The first revision may be zero. Revisions fit an unsigned 64-bit integer.
        """
        return _native.replace(self._state, source, revision, ids)

    def top(self):
        """Return a snapshot of (document ID, score), score descending, ID ascending."""
        return _native.top(self._state)
