#include "../include/rankpatch.hpp"
#include "../bench/indexed.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Id = std::uint32_t;
using List = std::vector<Id>;
using Ranking = std::vector<std::pair<Id, std::uint64_t>>;
constexpr std::size_t depth = 32, k = 10;
#ifdef RANKPATCH_REPLAY_SQLITE
#include "sqlite_reference.hpp"
#endif

struct Update {
    Id source;
    std::uint64_t version;
    List list;
};

struct Query {
    std::size_t sources, universe;
    std::vector<Update> updates;
};

bool better(const Ranking::value_type& a, const Ranking::value_type& b) {
    return a.second > b.second || (a.second == b.second && a.first < b.first);
}

List points() {
    List p(depth);
    for (std::size_t i = 0; i < depth; ++i) p[i] = static_cast<Id>(depth - i);
    return p;
}

// Two native baselines share the same read path. The incremental version credits
// only positions whose IDs changed, and the rebuild version sums all active lists.
template<bool incremental> class Reference {
    std::vector<List> lists_;
    std::vector<std::uint64_t> scores_;
    List weights_;
    Ranking output_;
    std::size_t k_;
public:
    Reference(std::size_t sources, std::size_t universe, List weights, std::size_t count)
        : lists_(sources), scores_(universe), weights_(std::move(weights)), k_(count) {
        output_.reserve(universe);
    }
    void replace(Id source, std::uint64_t, const List& list) {
        auto& old = lists_[source];
        if constexpr (incremental) {
            for (std::size_t i = 0; i < std::max(old.size(), list.size()); ++i) {
                if (i < old.size() && i < list.size() && old[i] == list[i]) continue;
                if (i < old.size()) scores_[old[i]] -= weights_[i];
                if (i < list.size()) scores_[list[i]] += weights_[i];
            }
        }
        old = list;
        if constexpr (!incremental) {
            std::fill(scores_.begin(), scores_.end(), 0);
            for (const auto& current : lists_)
                for (std::size_t i = 0; i < current.size(); ++i)
                    scores_[current[i]] += weights_[i];
        }
    }
    const Ranking& top() {
        output_.clear();
        for (Id id = 0; id < scores_.size(); ++id)
            if (scores_[id]) output_.emplace_back(id, scores_[id]);
        const auto keep = std::min(k_, output_.size());
        std::partial_sort(output_.begin(), output_.begin() + keep, output_.end(), better);
        output_.resize(keep);
        return output_;
    }
};

std::vector<Query> read_fixture(const char* file, bool progressive) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("Cannot read fixture");
    std::map<unsigned, std::vector<List>> runs;
    const std::array<std::string,6> methods = {
        "bm25","cosine_float","cosine_int8","hamming","bm25-hamming","hamming-cosine_float"};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        unsigned query;
        std::string method;
        if (!(row >> query >> method)) throw std::runtime_error("Bad row header");
        auto& lists = runs[query];
        if (lists.size() >= methods.size() || method != methods[lists.size()])
            throw std::runtime_error("Unexpected method");
        List list;
        Id id;
        while (row >> id) list.push_back(id);
        if (list.size() != depth) throw std::runtime_error("Wrong depth");
        lists.push_back(list);
    }
    std::vector<Query> queries;
    for (auto& [qid, lists] : runs) {
        if (lists.size() != methods.size()) throw std::runtime_error("Missing method");
        List universe;
        for (const auto& list : lists) universe.insert(universe.end(), list.begin(), list.end());
        std::sort(universe.begin(), universe.end());
        universe.erase(std::unique(universe.begin(),universe.end()), universe.end());
        for (auto& list : lists)
            for (auto& id : list)
                id = static_cast<Id>(std::lower_bound(universe.begin(), universe.end(), id) - universe.begin());
        Query query{progressive ? 6U : 3U, universe.size(), {}};
        if (progressive) {
            for (unsigned length : {4, 8, 16, 32})
                for (Id s = 0; s < lists.size(); ++s)
                    query.updates.push_back({s, length, List(lists[s].begin(), lists[s].begin()+length)});
        } else {
            // Stage 1 BM25 / int8 / Hamming. Stage 2 changes each source to a
            // published hybrid or reranked list. This schedule is simulated.
            const std::array<unsigned,6> order = {0,2,3,4,1,5};
            for (unsigned i = 0; i < order.size(); ++i)
                query.updates.push_back({i%3, i/3+1, lists[order[i]]});
        }
        queries.push_back(std::move(query));
    }
    if (queries.size() != 16) throw std::runtime_error("Unexpected fixture query count");
    return queries;
}

void verify(const std::vector<Query>& queries) {
    for (const auto& q : queries) {
        rankpatch::Index index(q.sources,q.universe,points(),k);
        IndexedHeap heap(q.sources,q.universe,points(),k);
        Reference<false> full(q.sources,q.universe,points(),k);
        Reference<true> delta(q.sources,q.universe,points(),k);
#ifdef RANKPATCH_REPLAY_SQLITE
        SqliteReference sql(q.sources,q.universe,points(),k);
#endif
        for (const auto& u : q.updates) {
            index.replace(u.source,u.version,u.list);
            heap.replace(u.source,u.version,u.list);
            full.replace(u.source,u.version,u.list);
            delta.replace(u.source,u.version,u.list);
            const auto& result = full.top();
#ifdef RANKPATCH_REPLAY_SQLITE
            sql.replace(u.source,u.version,u.list);
            if (sql.top() != result) throw std::runtime_error("SQLite result differs");
#endif
            if (index.top() != result || delta.top() != result || heap.top() != result)
                throw std::runtime_error("Runners disagree on an ordered (ID,score) result");
        }
    }
}

using Clock = std::chrono::steady_clock;
volatile std::uint64_t sink;

template<class Engine>
double sample(const std::vector<Query>& queries) {
    std::uint64_t checksum = 0, events = 0;
    Clock::duration total{};
    for (const auto& q : queries) {
        Engine engine(q.sources,q.universe,points(),k);
        const auto start = Clock::now();
        for (const auto& u : q.updates) {
            engine.replace(u.source,u.version,u.list);
            const auto& result = engine.top();
            for (const auto& [id, score] : result) checksum += (id+1)*score;
        }
        total += Clock::now()-start;
        events += q.updates.size();
    }
    sink = checksum;
    return std::chrono::duration<double,std::nano>(total).count()/events;
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: replay PATH/TO/scifact.txt\n"; return 1; }
#ifdef RANKPATCH_REPLAY_SQLITE
    std::cout << "SQLite " << sqlite3_libversion() << "\n";
#endif
    for (bool progressive : {true,false}) {
        const auto queries = read_fixture(argv[1],progressive);
        verify(queries);
#ifdef RANKPATCH_REPLAY_SQLITE
        constexpr std::size_t count = 5;
#else
        constexpr std::size_t count = 4;
#endif
        std::array<std::vector<double>,count> times;
        // Each sample averages the real fixture's updates; rotate implementation
        // order each round to spread CPU frequency and shared-host effects.
        for (int round = -6; round < 75; ++round)
            for (unsigned offset = 0; offset < count; ++offset) {
                auto m = (round+6+offset)%count;
                double t;
                switch (m) {
                    case 0: t = sample<rankpatch::Index>(queries); break;
                    case 1: t = sample<Reference<false>>(queries); break;
                    case 2: t = sample<Reference<true>>(queries); break;
                    case 3: t = sample<IndexedHeap>(queries); break;
#ifdef RANKPATCH_REPLAY_SQLITE
                    case 4: t = sample<SqliteReference>(queries); break;
#endif
                    default: throw std::logic_error("Unknown benchmark");
                }
                if (round >= 0) times[m].push_back(t);
            }
        std::cout << (progressive ? "progressive" : "refinement") << ": "
                  << queries.size() << " queries, " << queries.size()*queries[0].updates.size()
                  << " verified updates\n";
        const std::array<const char*,5> names = {"RankPatch","dense rebuild","dense delta","indexed heap","SQLite"};
        for (unsigned m=0;m<times.size();++m) {
            auto& t = times[m]; std::sort(t.begin(),t.end());
            std::cout << names[m] << " median " << std::fixed << std::setprecision(1)
                      << t[t.size()/2] << " ns per replace + top-10\n";
        }
    }
}
