#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <chrono>
#include <cstdint>
#include <utility>
using namespace std;

// Поліноміальний хеш рядка: h(s) = sum a_i * p^i, натуральне переповнення за модулем 2^64 (a_i -- код символу + 1).
// Довжина рядка обмежена 15 символами -> обчислення хешу є O(1).
static uint64_t BASE_P; // випадкова непарна основа, обирається один раз при старті

static inline uint64_t polyHash(const string& s) {
    uint64_t h = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i) {
        h = h * BASE_P + (uint64_t)((unsigned char)s[i]) + 1;
    }
    return h;
}

// Перевірка рядка за умовою: непорожній, довжина до 15, лише малі латинські літери a-z.
static const size_t kMaxLen = 15;

static bool isValidString(const string& s) {
    if (s.empty() || s.size() > kMaxLen) return false;
    for (char c : s)
        if (c < 'a' || c > 'z') return false;
    return true;
}

// Хеш-таблиця з методом ланцюгів. Зберігає для кожного унікального рядка його лічильник входжень (тобто це мультимножина).
class HashTable {
public:
    explicit HashTable(size_t initialBuckets = 1024) {
        buckets_.resize(nextPrime(initialBuckets));
        size_ = 0;
    }

    // Додати одне входження рядка s. Повертає новий лічильник, або 0, якщо рядок некоректний (не додається).
    int insert(const string& s) {
        if (!isValidString(s)) return 0;
        uint64_t h = polyHash(s);
        if (loadFactor() > kMaxLoad) rehash();
        auto& bucket = buckets_[h % buckets_.size()];
        for (auto& e : bucket) {
            if (e.h == h && e.key == s) {
                return ++e.cnt;
            }
        }
        bucket.push_back(Entry{ s, h, 1 });
        ++size_;
        return 1;
    }

    // Вилучити одне входження рядка s, якщо він є.
    // Повертає лічильник, що залишився, або -1, якщо s не було в множині.
    int erase(const string& s) {
        uint64_t h = polyHash(s);
        auto& bucket = buckets_[h % buckets_.size()];
        for (size_t i = 0; i < bucket.size(); ++i) {
            if (bucket[i].h == h && bucket[i].key == s) {
                int remaining = --bucket[i].cnt;
                if (remaining == 0) {
                    bucket[i] = std::move(bucket.back());
                    bucket.pop_back();
                    --size_;
                }
                return remaining;
            }
        }
        return -1;
    }

    bool contains(const string& s) const {
        uint64_t h = polyHash(s);
        const auto& bucket = buckets_[h % buckets_.size()];
        for (const auto& e : bucket)
            if (e.h == h && e.key == s) return true;
        return false;
    }

    // Викликає f(рядок, кількість) для кожного рядка з лічильником > 1.
    template <typename F>
    void forEachDuplicate(F f) const {
        for (const auto& bucket : buckets_)
            for (const auto& e : bucket)
                if (e.cnt > 1) f(e.key, e.cnt);
    }

    size_t distinctCount() const { return size_; }

private:
    struct Entry {
        string key;
        uint64_t h;
        int cnt;
    };

    vector<vector<Entry>> buckets_;
    size_t size_;
    static constexpr double kMaxLoad = 1.0; // поріг для рехешування

    double loadFactor() const {
        return (double)(size_ + 1) / (double)buckets_.size();
    }

    static bool isPrime(size_t x) {
        if (x < 2) return false;
        for (size_t d = 2; d * d <= x; ++d)
            if (x % d == 0) return false;
        return true;
    }
    static size_t nextPrime(size_t x) {
        while (!isPrime(x)) ++x;
        return x;
    }

    void rehash() {
        size_t newSize = nextPrime(buckets_.size() * 2);
        vector<vector<Entry>> newBuckets(newSize);
        for (auto& bucket : buckets_)
            for (auto& e : bucket)
                newBuckets[e.h % newSize].push_back(std::move(e));
        buckets_ = std::move(newBuckets);
    }
};

// Вбудований генератор: створює словник з dictSize унікальних-ish рядків (довжина 1..15, символи a-z) 
// і виконує opCount випадкових операцій над таблицею.
static void runGenerator(HashTable& table, long long dictSize,
    long long opCount, unsigned long long seed) {
    mt19937_64 rng(seed);
    uniform_int_distribution<int> lenDist(1, 15);
    uniform_int_distribution<int> chDist(0, 25);

    vector<string> dict;
    dict.reserve(static_cast<size_t>(dictSize));
    for (long long i = 0; i < dictSize; ++i) {
        int len = lenDist(rng);
        string s;
        s.reserve(static_cast<size_t>(len));
        for (int j = 0; j < len; ++j)
            s.push_back(static_cast<char>('a' + chDist(rng)));
        dict.push_back(std::move(s));
    }

    uniform_int_distribution<int> opDist(0, 99);
    uniform_int_distribution<long long> idxDist(0, dictSize - 1);

    long long plusCnt = 0, minusCnt = 0, queryCnt = 0, yesCnt = 0, noCnt = 0;

    auto t0 = chrono::steady_clock::now();
    for (long long i = 0; i < opCount; ++i) {
        int r = opDist(rng);
        const string& s = dict[static_cast<size_t>(idxDist(rng))];
        if (r < 70) {
            table.insert(s);
            ++plusCnt;
        }
        else if (r < 90) {
            if (table.contains(s)) ++yesCnt; else ++noCnt;
            ++queryCnt;
        }
        else {
            table.erase(s);
            ++minusCnt;
        }
    }
    auto t1 = chrono::steady_clock::now();
    double ms = chrono::duration<double, std::milli>(t1 - t0).count();

    cout << "Generated and executed " << opCount << " operations (seed=" << seed << "):\n"
        << "  +: " << plusCnt << ", -: " << minusCnt << ", ?: " << queryCnt
        << " (yes: " << yesCnt << ", no: " << noCnt << ")\n"
        << "  execution time: " << ms << " ms\n"
        << "  distinct strings in the table now: " << table.distinctCount() << "\n";
}

int main() {
    ios_base::sync_with_stdio(false);

    mt19937_64 rng(chrono::steady_clock::now().time_since_epoch().count());
    BASE_P = rng() | 1ULL; // непарне число

    HashTable table(1 << 10);

    cerr << "Commands: + s | - s | ? s | gen <dict_size> <ops> [seed] | # | #all\n";

    bool printAll = false;
    string line;
    while (getline(cin, line)) {
        istringstream iss(line);
        string tok;
        if (!(iss >> tok)) continue; // порожній рядок

        if (tok == "#" || tok == "#all") {
            printAll = (tok == "#all");
            break;
        }

        if (tok == "gen") {
            long long dictSize = 0, opCount = 0;
            if (!(iss >> dictSize >> opCount) || dictSize < 1 || opCount < 1) {
                cout << "Usage: gen <dict_size> <ops_count> [seed]\n";
                continue;
            }
            unsigned long long seed;
            if (!(iss >> seed))
                seed = static_cast<unsigned long long>(
                    chrono::steady_clock::now().time_since_epoch().count());
            runGenerator(table, dictSize, opCount, seed);
            continue;
        }

        if (tok == "+" || tok == "-" || tok == "?") {
            string s;
            if (!(iss >> s)) {
                cout << "A string is required after '" << tok << "'\n";
                continue;
            }
            if (tok == "+") {
                if (table.insert(s) == 0)
                    cout << "Invalid string: only lowercase Latin letters a-z, length 1-" << kMaxLen << "\n";
            }
            else if (tok == "-") {
                table.erase(s);
            }
            else {
                cout << (table.contains(s) ? "yes\n" : "no\n");
            }
            continue;
        }

        cout << "Unknown command: " << tok << "\n";
    }

    // пошук усіх дублікатів
    vector<pair<string, int>> duplicates;
    duplicates.reserve(table.distinctCount());
    table.forEachDuplicate([&](const string& key, int cnt) {
        duplicates.emplace_back(key, cnt);
        });
    sort(duplicates.begin(), duplicates.end());

    const size_t kShowLimit = 100;
    size_t shown = printAll ? duplicates.size()
        : min(duplicates.size(), kShowLimit);

    cout << "=== Duplicates (" << duplicates.size() << ") ===\n";
    for (size_t i = 0; i < shown; ++i)
        cout << duplicates[i].first << " " << duplicates[i].second << "\n";
    if (shown < duplicates.size())
        cout << "... " << (duplicates.size() - shown)
        << " more not shown (finish with #all to see everything)\n";

    return 0;
}