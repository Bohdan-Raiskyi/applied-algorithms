#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <random>
#include <chrono>
#include <unordered_set>
#include <algorithm>
#include <memory>
using namespace std;

static const double kMaxElements = 1000000.0; // n: максимальна кількість елементів
static const double kFalsePositive = 0.01;    // p: потрібна ймовірність помилки
static const size_t kMaxLen = 15;             // максимальна довжина рядка

// Рядок: непорожній, довжина <= 15, лише латинські літери (малі або великі).
static bool isValidString(const string& s) {
    if (s.empty() || s.size() > kMaxLen) return false;
    for (char c : s) {
        bool lower = (c >= 'a' && c <= 'z');
        bool upper = (c >= 'A' && c <= 'Z');
        if (!lower && !upper) return false;
    }
    return true;
}

struct FilterParams {
    size_t m;
    int l;
};

static double theoreticalFP(double n, size_t m, int l) {
    return pow(1.0 - exp(-static_cast<double>(l) * n / static_cast<double>(m)), l);
}

static FilterParams computeParams(double n, double p) {
    const double ln2 = log(2.0);
    double mReal = -n * log(p) / (ln2 * ln2);          // (4.1)
    int l = static_cast<int>(floor(mReal / n * ln2 + 0.5)); // (4.2), округлення
    if (l < 1) l = 1;
    size_t m = static_cast<size_t>(ceil(mReal));
    while (theoreticalFP(n, m, l) > p) m += m / 1000 + 1;
    return FilterParams{ m, l };
}

class HashFamily {
public:
    HashFamily(int l, uint64_t seed) : bases_(static_cast<size_t>(l)) {
        mt19937_64 rng(seed);
        for (auto& b : bases_) b = rng() | 1ULL; // непарна основа
    }

    int count() const { return static_cast<int>(bases_.size()); }

    uint64_t hash(int i, const string& s) const {
        const uint64_t p = bases_[static_cast<size_t>(i)];
        uint64_t h = 0; // арифметика за модулем 2^64 (природне переповнення)
        for (size_t k = s.size(); k-- > 0;)
            h = h * p + static_cast<uint64_t>(static_cast<unsigned char>(s[k])) + 1;
        return mix(h);
    }

private:
    static uint64_t mix(uint64_t x) {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    vector<uint64_t> bases_;
};


class IFilter {
public:
    virtual ~IFilter() {}
    virtual void add(const string& s) = 0;
    virtual bool contains(const string& s) const = 0;
    // Вилучення: повертає true, якщо вилучення виконано. Класичний фільтр його не підтримує.
    virtual bool remove(const string&) { return false; }
    virtual bool supportsRemove() const { return false; }
    virtual double fillRatio() const = 0;   // частка зайнятих позицій
    virtual size_t memoryBytes() const = 0;
    virtual const char* name() const = 0;
    virtual size_t positions() const = 0;   // m
    virtual int hashCount() const = 0;      // l
};

static int popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<int>((x * 0x0101010101010101ULL) >> 56);
}

class BloomFilter : public IFilter {
public:
    BloomFilter(size_t m, int l, uint64_t seed)
        : m_(m), hf_(l, seed), bits_((m + 63) / 64, 0ULL) {
    }

    void add(const string& s) override {
        for (int i = 0; i < hf_.count(); ++i) {
            size_t pos = static_cast<size_t>(hf_.hash(i, s) % m_);
            bits_[pos >> 6] |= (1ULL << (pos & 63));
        }
    }

    bool contains(const string& s) const override {
        for (int i = 0; i < hf_.count(); ++i) {
            size_t pos = static_cast<size_t>(hf_.hash(i, s) % m_);
            if (((bits_[pos >> 6] >> (pos & 63)) & 1ULL) == 0ULL) return false;
        }
        return true;
    }

    double fillRatio() const override {
        size_t ones = 0;
        for (uint64_t w : bits_) ones += static_cast<size_t>(popcount64(w));
        return static_cast<double>(ones) / static_cast<double>(m_);
    }

    size_t memoryBytes() const override { return bits_.size() * sizeof(uint64_t); }
    const char* name() const override { return "classic Bloom filter (bit array)"; }
    size_t positions() const override { return m_; }
    int hashCount() const override { return hf_.count(); }

private:
    size_t m_;
    HashFamily hf_;
    vector<uint64_t> bits_;
};


class CountingBloomFilter : public IFilter {
public:
    CountingBloomFilter(size_t m, int l, uint64_t seed)
        : m_(m), hf_(l, seed), cnt_(m, 0) {
    }

    void add(const string& s) override {
        for (int i = 0; i < hf_.count(); ++i) {
            uint8_t& c = cnt_[static_cast<size_t>(hf_.hash(i, s) % m_)];
            if (c < 255) ++c; // насичення
        }
    }

    bool contains(const string& s) const override {
        for (int i = 0; i < hf_.count(); ++i)
            if (cnt_[static_cast<size_t>(hf_.hash(i, s) % m_)] == 0) return false;
        return true;
    }

    bool remove(const string& s) override {
        if (!contains(s)) return false; // точно відсутній -- нічого не чіпаємо
        for (int i = 0; i < hf_.count(); ++i) {
            uint8_t& c = cnt_[static_cast<size_t>(hf_.hash(i, s) % m_)];
            if (c > 0 && c < 255) --c;
        }
        return true;
    }

    bool supportsRemove() const override { return true; }

    double fillRatio() const override {
        size_t nonzero = 0;
        for (uint8_t c : cnt_) if (c != 0) ++nonzero;
        return static_cast<double>(nonzero) / static_cast<double>(m_);
    }

    size_t memoryBytes() const override { return cnt_.size() * sizeof(uint8_t); }
    const char* name() const override { return "counting Bloom filter (8-bit counters)"; }
    size_t positions() const override { return m_; }
    int hashCount() const override { return hf_.count(); }

private:
    size_t m_;
    HashFamily hf_;
    vector<uint8_t> cnt_;
};

static unique_ptr<IFilter> makeFilter(bool counting, size_t m, int l, uint64_t seed) {
    if (counting) return unique_ptr<IFilter>(new CountingBloomFilter(m, l, seed));
    return unique_ptr<IFilter>(new BloomFilter(m, l, seed));
}

static void printParams(const IFilter& f) {
    cerr << "Filter: " << f.name() << "\n"
        << "  capacity n = " << static_cast<long long>(kMaxElements)
        << ", target false positive p = " << kFalsePositive * 100 << "%\n"
        << "  m = " << f.positions() << " positions"
        << " (" << static_cast<double>(f.memoryBytes()) / (1024.0 * 1024.0) << " MiB), "
        << "l = " << f.hashCount() << " hash functions\n"
        << "  theoretical false positive rate at n elements: "
        << theoreticalFP(kMaxElements, f.positions(), f.hashCount()) * 100 << "%\n";
}

static void printStat(const IFilter& f) {
    double fill = f.fillRatio();
    cout << "m = " << f.positions() << ", l = " << f.hashCount()
        << ", memory = " << static_cast<double>(f.memoryBytes()) / (1024.0 * 1024.0) << " MiB\n"
        << "fill ratio = " << fill * 100 << "%\n"
        << "estimated false positive rate now = " << pow(fill, f.hashCount()) * 100 << "%\n";
}

static string randomString(mt19937_64& rng) {
    uniform_int_distribution<int> lenDist(1, static_cast<int>(kMaxLen));
    uniform_int_distribution<int> chDist(0, 25);
    int len = lenDist(rng);
    string s(static_cast<size_t>(len), 'a');
    for (int j = 0; j < len; ++j)
        s[static_cast<size_t>(j)] = static_cast<char>('a' + chDist(rng));
    return s;
}

static void runSelfTest(bool counting, size_t m, int l,
    long long N, long long Q, uint64_t seed) {
    typedef chrono::steady_clock Clock;
    mt19937_64 rng(seed);
    unique_ptr<IFilter> f = makeFilter(counting, m, l, seed + 1);

    unordered_set<string> ref;
    vector<string> items;
    ref.reserve(static_cast<size_t>(N));
    items.reserve(static_cast<size_t>(N));
    auto t0 = Clock::now();

    while (static_cast<long long>(items.size()) < N) {
        string s = randomString(rng);
        if (ref.insert(s).second) {
            f->add(s);
            items.push_back(s);
        }
    }
    auto t1 = Clock::now();

    // 2. Хибнонегативних бути не повинно: усі вставлені мають давати true.
    long long falseNeg = 0;
    for (const string& s : items) if (!f->contains(s)) ++falseNeg;

    // 3. Хибнопозитивні: Q випадкових рядків, яких немає в еталоні.
    long long absent = 0, falsePos = 0;
    auto t2 = Clock::now();
    for (long long i = 0; i < Q; ++i) {
        string s = randomString(rng);
        if (ref.count(s)) continue; // такий рядок справді доданий -- пропускаємо
        ++absent;
        if (f->contains(s)) ++falsePos;
    }
    auto t3 = Clock::now();

    double addMs = chrono::duration<double, milli>(t1 - t0).count();
    double qMs = chrono::duration<double, milli>(t3 - t2).count();
    double fill = f->fillRatio();

    cout << "Self-test (" << f->name() << ", seed=" << seed << "):\n"
        << "  inserted " << items.size() << " distinct strings, time " << addMs << " ms\n"
        << "  false negatives: " << falseNeg << " (must be 0)\n"
        << "  queried " << absent << " absent strings, false positives: " << falsePos
        << " = " << (absent ? 100.0 * static_cast<double>(falsePos) / static_cast<double>(absent) : 0.0)
        << "% (target " << kFalsePositive * 100 << "%), time " << qMs << " ms\n"
        << "  fill ratio " << fill * 100 << "%, estimate from fill ratio "
        << pow(fill, l) * 100 << "%\n";

    // 4. Для лічильникового фільтра: вилучаємо половину і перевіряємо, що
    //    решта (яку не чіпали) усе ще присутня -- хибнонегативних немає.
    if (f->supportsRemove()) {
        size_t half = items.size() / 2;
        long long removed = 0;
        for (size_t i = 0; i < half; ++i) if (f->remove(items[i])) ++removed;
        long long fnAfter = 0, stillThere = 0;
        for (size_t i = half; i < items.size(); ++i) {
            if (!f->contains(items[i])) ++fnAfter;
        }
        for (size_t i = 0; i < half; ++i) if (f->contains(items[i])) ++stillThere;
        cout << "  removed " << removed << " strings; false negatives among the untouched half: "
            << fnAfter << " (must be 0)\n"
            << "  removed strings still reported present (false positives): " << stillThere << "\n";
    }
}

// ---------------------------------------------------------------------
int main(int argc, char* argv[]) {
    ios_base::sync_with_stdio(false);

    bool counting = false;
    string path;
    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        if (a == "--counting" || a == "-c") counting = true;
        else path = a;
    }

    ifstream file;
    if (!path.empty()) {
        file.open(path.c_str());
        if (!file) {
            cerr << "Cannot open input file: " << path << "\n";
            return 1;
        }
    }
    istream& in = path.empty() ? cin : static_cast<istream&>(file);

    FilterParams prm = computeParams(kMaxElements, kFalsePositive);
    uint64_t seed = static_cast<uint64_t>(
        chrono::steady_clock::now().time_since_epoch().count());
    unique_ptr<IFilter> filter = makeFilter(counting, prm.m, prm.l, seed);

    printParams(*filter);
    cerr << "Commands: + s | ? s | " << (counting ? "- s | " : "")
        << "stat | test N Q [seed] | #\n";

    long long ops = 0, invalid = 0, deleteIgnored = 0;
    auto tStart = chrono::steady_clock::now();

    string line;
    while (getline(in, line)) {
        istringstream iss(line);
        string tok;
        if (!(iss >> tok)) continue; // порожній рядок
        if (tok == "#") break;

        if (tok == "stat") {
            printStat(*filter);
            continue;
        }
        if (tok == "test") {
            long long N = 0, Q = 0;
            if (!(iss >> N >> Q) || N < 1 || Q < 1) {
                cout << "Usage: test <N> <Q> [seed]\n";
                continue;
            }
            unsigned long long s2;
            if (!(iss >> s2)) s2 = static_cast<unsigned long long>(
                chrono::steady_clock::now().time_since_epoch().count());
            runSelfTest(counting, prm.m, prm.l, N, Q, s2);
            continue;
        }

        if (tok == "+" || tok == "-" || tok == "?") {
            string s;
            iss >> s;
            ++ops;
            bool ok = isValidString(s);
            if (!ok) ++invalid;

            if (tok == "+") {
                if (ok) filter->add(s);
            }
            else if (tok == "?") {
                cout << ((ok && filter->contains(s)) ? 'Y' : 'N') << '\n';
            }
            else { // "-"
                if (!filter->supportsRemove()) ++deleteIgnored;
                else if (ok) filter->remove(s);
            }
            continue;
        }

        cout << "Unknown command: " << tok << "\n";
    }

    auto tEnd = chrono::steady_clock::now();
    double ms = chrono::duration<double, milli>(tEnd - tStart).count();
    cerr << "Processed " << ops << " operations in " << ms << " ms";
    if (invalid) cerr << "; invalid strings ignored: " << invalid;
    if (deleteIgnored)
        cerr << "; deletions ignored: " << deleteIgnored
        << " (classic filter does not support them, use --counting)";
    cerr << "\n";
    return 0;
}