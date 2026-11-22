// Optional replay comparator; uses the fixture's List, Ranking and Id aliases.
#include <sqlite3.h>

class SqliteReference {
    sqlite3* db_ = nullptr;
    sqlite3_stmt* update_ = nullptr;
    sqlite3_stmt* read_ = nullptr;
    std::vector<List> lists_;
    List weights_;
    std::vector<std::int64_t> deltas_;
    Ranking result_;
    void check(int rc, int expected = SQLITE_OK) {
        if (rc != expected) throw std::runtime_error(sqlite3_errmsg(db_));
    }
    void exec(const char* sql) {
        check(sqlite3_exec(db_, sql, nullptr, nullptr, nullptr));
    }
    void prepare(const char* sql, sqlite3_stmt** stmt) {
        check(sqlite3_prepare_v2(db_, sql, -1, stmt, nullptr));
    }
    void close() {
        sqlite3_finalize(read_);
        sqlite3_finalize(update_);
        sqlite3_close(db_);
    }
public:
    SqliteReference(std::size_t sources, std::size_t universe, List weights, std::size_t count)
        : lists_(sources), weights_(std::move(weights)), deltas_(universe) {
        try {
            check(sqlite3_open(":memory:", &db_));
            exec("BEGIN; CREATE TABLE docs(id INTEGER PRIMARY KEY,score INTEGER NOT NULL);");
            for (std::size_t id = 0; id < universe; ++id) {
                auto sql = "INSERT INTO docs VALUES(" + std::to_string(id) + ",0)";
                exec(sql.c_str());
            }
            exec("CREATE INDEX ranking ON docs(score DESC,id ASC) WHERE score>0;");
            prepare("UPDATE docs SET score=score+?1 WHERE id=?2", &update_);
            prepare("SELECT id,score FROM docs WHERE score>0 ORDER BY score DESC,id ASC LIMIT ?1", &read_);
            check(sqlite3_bind_int64(read_,1,static_cast<sqlite3_int64>(count)));
            result_.reserve(count);
        } catch (...) {
            close();
            throw;
        }
    }
    SqliteReference(const SqliteReference&) = delete;
    SqliteReference& operator=(const SqliteReference&) = delete;
    ~SqliteReference() { close(); }
    void replace(Id source, std::uint64_t, const List& list) {
        auto& old = lists_[source];
        for (std::size_t i = 0; i < old.size(); ++i) deltas_[old[i]] -= weights_[i];
        for (std::size_t i = 0; i < list.size(); ++i) deltas_[list[i]] += weights_[i];
        old = list;
        for (Id id = 0; id < deltas_.size(); ++id) {
            if (deltas_[id] == 0) continue;
            check(sqlite3_bind_int64(update_,1,deltas_[id]));
            check(sqlite3_bind_int64(update_,2,id));
            check(sqlite3_step(update_),SQLITE_DONE);
            check(sqlite3_reset(update_));
            deltas_[id] = 0;
        }
    }
    const Ranking& top() {
        result_.clear();
        int rc;
        while ((rc=sqlite3_step(read_)) == SQLITE_ROW) {
            result_.emplace_back(static_cast<Id>(sqlite3_column_int64(read_,0)),
                                 static_cast<std::uint64_t>(sqlite3_column_int64(read_,1)));
        }
        check(rc,SQLITE_DONE);
        check(sqlite3_reset(read_));
        return result_;
    }
};
