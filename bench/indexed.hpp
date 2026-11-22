#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// Independent benchmark comparator. Keep winners in display order and all
// remaining active IDs in an indexed max heap. This implementation assumes
// the benchmark's validated unique IDs and positive rank points.
class IndexedHeap {
    using Id = std::uint32_t;
    using Score = std::uint64_t;
    using Entry = std::pair<Id, Score>;
    using List = std::vector<Id>;
    static constexpr std::size_t missing = std::numeric_limits<std::size_t>::max();

    static bool better(const Entry& a, const Entry& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    }

public:
    IndexedHeap(std::size_t sources, std::size_t universe, const List& points, std::size_t k)
        : k_(k), points_(points), ballots_(sources), revisions_(sources),
          initialized_(sources), scores_(universe), positions_(universe, missing),
          deltas_(universe), marked_(universe) {
        winners_.reserve(std::min(k, universe));
        others_.reserve(std::min(universe, sources * points.size()));
        touched_.reserve(2 * points.size());
    }

    bool replace(std::size_t source, std::uint64_t revision, const List& ids) {
        if (initialized_[source] && revision <= revisions_[source]) return false;
        auto& old = ballots_[source];
        touched_.clear();
        for (std::size_t i = 0; i < std::max(old.size(), ids.size()); ++i) {
            if (i < old.size() && i < ids.size() && old[i] == ids[i]) continue;
            const auto p = static_cast<std::int64_t>(points_[i]);
            if (i < old.size()) accumulate(old[i], -p);
            if (i < ids.size()) accumulate(ids[i], p);
        }
        for (const auto id : touched_) {
            const auto delta = deltas_[id];
            marked_[id] = false;
            deltas_[id] = 0;
            if (!delta) continue;
            const auto before = scores_[id];
            const auto after = delta < 0 ? before - Score(-delta) : before + Score(delta);
            scores_[id] = after;
            update(id, before, after);
        }
        old = ids;
        revisions_[source] = revision;
        initialized_[source] = true;
        return true;
    }

    const std::vector<Entry>& top() const { return winners_; }

private:
    void accumulate(Id id, std::int64_t delta) {
        if (!marked_[id]) {
            touched_.push_back(id);
            marked_[id] = true;
        }
        deltas_[id] += delta;
    }

    void put(std::size_t i, Entry e) {
        others_[i] = e;
        positions_[e.first] = i;
    }

    void place(std::size_t hole, Entry e) {
        // Repair an arbitrary replacement by following one root-to-leaf path.
        if (hole && better(e, others_[(hole - 1) / 2])) {
            do {
                const auto parent = (hole - 1) / 2;
                put(hole, others_[parent]);
                hole = parent;
            } while (hole && better(e, others_[(hole - 1) / 2]));
        } else {
            for (auto child = 2 * hole + 1; child < others_.size(); child = 2 * hole + 1) {
                if (child + 1 < others_.size() && better(others_[child + 1], others_[child]))
                    ++child;
                if (!better(others_[child], e)) break;
                put(hole, others_[child]);
                hole = child;
            }
        }
        put(hole, e);
    }

    void push_other(Entry e) {
        const auto hole = others_.size();
        others_.push_back(e);
        place(hole, e);
    }

    void drop_other(std::size_t i) {
        positions_[others_[i].first] = missing;
        const auto last = others_.back();
        others_.pop_back();
        if (i < others_.size()) place(i, last);
    }

    void insert_winner(Entry e) {
        winners_.insert(std::lower_bound(winners_.begin(), winners_.end(), e, better), e);
    }

    void update(Id id, Score before, Score after) {
        const Entry e{id, after};
        const auto pos = positions_[id];
        if (before && pos == missing) {
            auto it = std::lower_bound(winners_.begin(), winners_.end(), Entry{id, before}, better);
            winners_.erase(it);
            if (!others_.empty() && (!after || better(others_.front(), e))) {
                const auto promoted = others_.front();
                if (after) {
                    positions_[promoted.first] = missing;
                    place(0, e);
                } else {
                    drop_other(0);
                }
                insert_winner(promoted);
            } else if (after) {
                insert_winner(e);
            }
            return;
        }
        if (!after) {
            drop_other(pos);
            return;
        }
        if (winners_.size() < k_) {
            insert_winner(e);
        } else if (k_ && better(e, winners_.back())) {
            const auto demoted = winners_.back();
            winners_.pop_back();
            if (pos != missing) {
                positions_[id] = missing;
                place(pos, demoted);
            } else {
                push_other(demoted);
            }
            insert_winner(e);
        } else if (pos == missing) {
            push_other(e);
        } else {
            place(pos, e);
        }
    }

    std::size_t k_;
    List points_;
    std::vector<List> ballots_;
    std::vector<std::uint64_t> revisions_;
    std::vector<std::uint8_t> initialized_;
    std::vector<Score> scores_;
    std::vector<std::size_t> positions_;
    std::vector<std::int64_t> deltas_;
    std::vector<std::uint8_t> marked_;
    List touched_;
    std::vector<Entry> winners_;
    std::vector<Entry> others_;
};
