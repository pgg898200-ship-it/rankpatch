# Stream a fused search result with LangChain

Show the first search results while the remaining retrievers work. This example queries titles and bodies in parallel, publishes a combined top three on each update, and revises it when a phrase reranker finishes. It runs on six local documents and requires no model or API key.

From the repository root, install the RankPatch wheel and the example dependencies. For platforms without a matching wheel, use `python -m pip install .` with a C++17 compiler for the first step.

```sh
python -m pip install --no-index --only-binary=:all: --find-links https://github.com/pgg898200-ship-it/rankpatch/releases/expanded_assets/v0.2.0 rankpatch==0.2.0
python -m pip install -r integrations/langchain/requirements.txt
python -m integrations.langchain.stream_search
python -m unittest integrations.langchain.test_stream_search
```

For the query `prefix cache`, the title search first favors **Prefix cache routing**. Its reranker promotes **Cache eviction** because the document body contains the exact phrase. Body search also ranks **Cache eviction** first. The final snapshot awards it 6 points, 3 from each source. Intermediate snapshots follow completion order.

## Connect your retrievers

With the wheel and example dependencies installed, copy [stream_search.py](stream_search.py) into your application. Run `python stream_search.py` there to replay the six-document example. The file imports the catalog adapter directly from the wheel.

In your query coordinator, pass `stream_top` a mapping of source names to LangChain runnables. Wrap a retriever that returns one list of LangChain documents like this.

```python
from langchain_core.runnables import RunnableLambda
from stream_search import Revision, stream_top

sources = {
    "lexical": lexical_retriever | RunnableLambda(lambda docs: Revision(0, docs)),
    "vector": vector_retriever | RunnableLambda(lambda docs: Revision(0, docs)),
}
async for source, version, hits in stream_top(query, sources, catalog, [3, 2, 1], 3):
    render_results([hit.document for hit in hits])
```

Set a stable `Document.id` on each result and keep a catalog mapping those IDs to the original documents. Each list contains distinct IDs and fits the rank-point table. A source can emit multiple complete lists with increasing versions, as `title_updates` does. Send an empty list to withdraw it. A late or repeated version leaves the current ranking in place. The coordinator returns canonical documents with their original text and metadata, even when a retriever works on a different representation.

The example uses [LangChain RunnableParallel](https://reference.langchain.com/python/langchain-core/runnables/base/RunnableParallel) to stream branch outputs and the [packaged RankPatch catalog adapter](../../python/rankpatch/catalog.py) to apply revisions. The output sums integer rank points and breaks ties by ascending catalog ID.
