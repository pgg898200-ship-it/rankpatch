#ifndef RANKPATCH_STEADY_HPP
#define RANKPATCH_STEADY_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rankpatch {

// Eagerly maintains sorted winners and an indexed max heap of all runners-up.
// Replacing a ballot costs O(old depth + new depth + changed * (k + log(A+1)))
// for A active documents, including worst-case winner shifts. Reading the
// borrowed vector costs O(1). Synchronize shared access. A mutation can
// invalidate an earlier borrowed result.
class SteadyIndex {
public:
    using Row = std::pair<std::uint32_t, std::uint64_t>;

    SteadyIndex(std::size_t sources, std::size_t universe,
                std::vector<std::uint32_t> rank_points, std::size_t k)
        : k_(std::min(k, universe)), points_(std::move(rank_points)) {
        if (universe && universe - 1 > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("document universe exceeds uint32 IDs");
        std::uint32_t maximum = 0;
        for (auto point : points_) {
            if (!point) throw std::invalid_argument("rank points must be positive");
            maximum = std::max(maximum, point);
        }
        if (maximum && sources > std::numeric_limits<std::uint64_t>::max() / maximum)
            throw std::overflow_error("source count times maximum rank points overflows");

        const auto depth = std::min(universe, points_.size());
        const auto capacity = depth && sources > universe / depth ? universe : sources * depth;
        sources_.resize(sources);
        incoming_.resize(universe);
        if (k_) {
            scores_.resize(universe);
            positions_.resize(universe, missing);
            winners_.reserve(std::min(k_, capacity));
            others_.reserve(capacity > k_ ? capacity - k_ : 0);
        }
    }

    bool is_fresh(std::size_t source, std::uint64_t revision) const {
        if (source >= sources_.size()) throw std::out_of_range("source ID");
        const auto& ballot = sources_[source];
        return !ballot.seen || revision > ballot.revision;
    }

    // Revision zero is valid. Strictly older revisions and replays are ignored
    // before payload validation. Rejected fresh ballots leave all scores and
    // the source revision intact. Validating costs O(old depth + new depth); sources may
    // grow their ballot storage. Each changed score also repairs the ranking.
    bool replace(std::size_t source, std::uint64_t revision,
                 const std::vector<std::uint32_t>& ids) {
        if (!is_fresh(source, revision)) return false;
        auto& s = sources_[source];
        if (ids.size() > points_.size()) throw std::invalid_argument("ballot too long");
        if (ids.size() == s.ids.size()) {
            std::size_t i = 0;
            while (i < ids.size() && ids[i] == s.ids[i]) ++i;
            if (i == ids.size()) {
                s.seen = true;
                s.revision = revision;
                return true;
            }
            // A reversal of exactly two positions preserves the validated set
            // of IDs. Verify the suffix before updating either score.
            if (i + 1 < ids.size() && ids[i] == s.ids[i + 1] &&
                ids[i + 1] == s.ids[i] &&
                std::equal(ids.begin() + i + 2, ids.end(), s.ids.begin() + i + 2)) {
                if (points_[i] != points_[i + 1]) {
                    change(s.ids[i], points_[i], points_[i + 1]);
                    change(s.ids[i + 1], points_[i + 1], points_[i]);
                }
                std::swap(s.ids[i], s.ids[i + 1]);
                s.revision = revision;
                return true;
            }
        }
        s.ids.reserve(ids.size());
        // incoming_ is both the uniqueness check and a direct map from a
        // new document to its points. Zero is free between replacements.
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto id = ids[i];
            if (id >= incoming_.size() || incoming_[id]) {
                for (std::size_t j = 0; j < i; ++j) incoming_[ids[j]] = 0;
                if (id >= incoming_.size()) throw std::out_of_range("document ID");
                throw std::invalid_argument("duplicate document ID in ballot");
            }
            incoming_[id] = points_[i];
        }
        // A document moving to another position is touched once after its
        // old and new points are known. Removals precede new insertions.
        for (std::size_t i = 0; i < s.ids.size(); ++i) {
            const auto id = s.ids[i];
            const auto next = incoming_[id];
            incoming_[id] = 0;
            if (points_[i] != next) change(id, points_[i], next);
        }
        for (auto id : ids) {
            if (const auto next = incoming_[id]) {
                incoming_[id] = 0;
                change(id, 0, next);
            }
        }
        s.ids.assign(ids.begin(), ids.end());
        s.seen = true;
        s.revision = revision;
        return true;
    }

    const std::vector<Row>& top() const { return winners_; }

private:
    using Id = std::uint32_t;
    static constexpr std::size_t missing = std::numeric_limits<std::size_t>::max();

    struct Ballot {
        std::vector<Id> ids;
        std::uint64_t revision = 0;
        bool seen = false;
    };

    static bool better(const Row& a, const Row& b) {
        return a.second > b.second || (a.second == b.second && a.first < b.first);
    }

    void put(std::size_t i, Row e) {
        others_[i] = e;
        positions_[e.first] = i;
    }

    void place(std::size_t hole, Row e) {
        // Repair an arbitrary replacement by following one root-to-leaf path.
        if (hole && better(e, others_[(hole - 1) / 2])) {
            do {
                const auto parent = (hole - 1) / 2;
                put(hole, others_[parent]);
                hole = parent;
            } while (hole && better(e, others_[(hole - 1) / 2]));
        } else {
            while (hole < others_.size() / 2) {
                auto child = 2 * hole + 1;
                if (child + 1 < others_.size() && better(others_[child + 1], others_[child]))
                    ++child;
                if (!better(others_[child], e)) break;
                put(hole, others_[child]);
                hole = child;
            }
        }
        put(hole, e);
    }

    void push_other(Row e) {
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

    void insert_winner(Row e) {
        winners_.insert(std::lower_bound(winners_.begin(), winners_.end(), e, better), e);
    }

    void change(Id id, Id previous, Id next) {
        if (!k_) return;
        const auto before = scores_[id];
        const auto after = before - previous + next;
        scores_[id] = after;
        const Row e{id, after};
        const auto pos = positions_[id];
        if (before && pos == missing) {
            auto it = std::lower_bound(winners_.begin(), winners_.end(), Row{id, before}, better);
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
    std::vector<Id> points_;
    std::vector<Ballot> sources_;
    std::vector<Id> incoming_;
    std::vector<std::uint64_t> scores_;
    // positions_[id] is a heap offset for a runner-up; winners and inactive
    // IDs carry the sentinel. Their current score distinguishes those cases.
    std::vector<std::size_t> positions_;
    std::vector<Row> winners_, others_;
};

} // namespace rankpatch
#endif
