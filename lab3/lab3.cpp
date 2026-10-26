#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

constexpr int     WIDTH = 4;       // степінь поліному N (розрядність CRC)
constexpr uint8_t POLY_FULL = 0x13;    // 1 0011  = x^4 + x + 1 (з старшим бітом)
constexpr uint8_t POLY = 0x03;    //   0011  - поліном без старшого біта
constexpr uint8_t POLY_REFLECT = 0x0C;    //   1100  - дзеркальне відображення POLY
constexpr uint8_t CRC_MASK = 0x0F;    // маска 4-бітного регістру

// Параметри експерименту
constexpr size_t K_BITS = 1000;              // довжина повідомлення, біт
constexpr size_t K_BYTES = K_BITS / 8;        // = 125 байт
constexpr size_t EXPERIMENTS = 10000;             // кількість різних повідомлень
constexpr int    REPEATS = 20;                // скільки разів проганяємо всю серію

static_assert(K_BITS % 8 == 0, "Табличні алгоритми обробляють повідомлення цілими байтами");

using Table = std::array<uint8_t, 256>;

// ---------------------------------------------------------------------------
// Допоміжні функції
// ---------------------------------------------------------------------------
std::string toBin(uint32_t v, int bits)
{
    std::string s;
    for (int i = bits - 1; i >= 0; --i) s += ((v >> i) & 1) ? '1' : '0';
    return s;
}

std::string toHex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", v);
    return buf;
}

// Віддзеркалення молодших bits біт числа
uint32_t reflect(uint32_t v, int bits)
{
    uint32_t r = 0;
    for (int i = 0; i < bits; ++i)
        if (v & (1u << i)) r |= 1u << (bits - 1 - i);
    return r;
}

// Пакування послідовності бітів (0/1) у байти, старший біт байта - перший
std::vector<uint8_t> packBits(const std::vector<uint8_t>& bits)
{
    std::vector<uint8_t> bytes((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
        if (bits[i]) bytes[i / 8] |= uint8_t(0x80u >> (i % 8));
    return bytes;
}

// 1. Простий послідовний алгоритм
//    - регістр 4 біти, заповнений нулями;
//    - повідомлення доповнюється N = 4 нульовими бітами;
//    - регістр зсувається вліво, у молодший розряд заноситься черговий біт;
//    - якщо висунуто 1, регістр = регістр XOR поліном.
//    Біти беруться починаючи зі старшого біта кожного байта.
//    nbits - кількість бітів повідомлення (може бути не кратною 8).
uint8_t crcSimple(const uint8_t* data, size_t nbits)
{
    uint8_t reg = 0;
    const size_t total = nbits + WIDTH;            // + N доповнюючих нулів
    for (size_t i = 0; i < total; ++i)
    {
        uint8_t bit = 0;
        if (i < nbits) bit = (data[i >> 3] >> (7 - (i & 7))) & 1;

        uint8_t popped = (reg >> (WIDTH - 1)) & 1; // біт, що висувається
        reg = uint8_t(((reg << 1) | bit) & CRC_MASK);
        if (popped) reg ^= POLY;
    }
    return reg;
}

// 2. Прямий табличний алгоритм
//    Оскільки CRC 4-бітний, а обробка йде байтами, регістр тримаємо
//    "вирівняним" у старшій тетраді 8-бітної комірки (reg << 4), а поліном
//    відповідно зсунутий: POLY << 4 = 0x30.
//    table[i] - залишок від ділення числа i * x^4 на G(x) (у старшій тетраді).
//    Крок: індекс = (старший байт регістру) XOR (байт повідомлення);
//          регістр = table[індекс].
//    Результат збігається з простим алгоритмом (доповнення нулями
//    враховане самою схемою, дописувати нулі не потрібно).
Table makeDirectTable()
{
    Table t{};
    const uint8_t polyTop = uint8_t(POLY << (8 - WIDTH));   // 0x30
    for (int i = 0; i < 256; ++i)
    {
        uint8_t r = uint8_t(i);
        for (int b = 0; b < 8; ++b)
            r = (r & 0x80) ? uint8_t((r << 1) ^ polyTop) : uint8_t(r << 1);
        t[i] = r;
    }
    return t;
}

uint8_t crcTable(const uint8_t* data, size_t nbytes, const Table& t)
{
    uint8_t reg = 0;                                // CRC у старшій тетраді
    for (size_t i = 0; i < nbytes; ++i)
        reg = t[reg ^ data[i]];
    return uint8_t(reg >> (8 - WIDTH));
}

// 3. Дзеркальний послідовний алгоритм
//    Біти кожного байта обробляються від молодшого до старшого,
//    регістр зсувається вправо, використовується дзеркальний поліном 0xC.
//    Нулі в кінець повідомлення не дописуються.
uint8_t crcReflectedSimple(const uint8_t* data, size_t nbytes)
{
    uint8_t reg = 0;
    for (size_t i = 0; i < nbytes; ++i)
    {
        uint8_t byte = data[i];
        for (int b = 0; b < 8; ++b)
        {
            uint8_t fb = (reg ^ byte) & 1;          // біт зворотного зв'язку
            reg >>= 1;
            byte >>= 1;
            if (fb) reg ^= POLY_REFLECT;
        }
    }
    return reg;
}

// 4. Дзеркальний табличний алгоритм
//    Таблиця - дзеркальне відображення таблиці прямого алгоритму.
//    Крок: індекс = (молодший байт регістру) XOR (байт повідомлення);
//          регістр = (регістр >> 8) XOR table[індекс].
//    Для 4-бітного CRC (регістр >> 8) = 0, тому регістр = table[індекс].

Table makeReflectedTable()
{
    Table t{};
    for (int i = 0; i < 256; ++i)
    {
        uint8_t r = uint8_t(i);
        for (int b = 0; b < 8; ++b)
            r = (r & 1) ? uint8_t((r >> 1) ^ POLY_REFLECT) : uint8_t(r >> 1);
        t[i] = r;
    }
    return t;
}

uint8_t crcReflectedTable(const uint8_t* data, size_t nbytes, const Table& t)
{
    uint8_t reg = 0;
    for (size_t i = 0; i < nbytes; ++i)
        reg = t[reg ^ data[i]];
    return reg;
}

// Вивід таблиці 16 x 16
void printTable(const Table& t, const char* title, bool shiftDown)
{
    std::cout << title << "\n     ";
    for (int c = 0; c < 16; ++c) std::cout << " _" << std::hex << std::uppercase << c;
    std::cout << "\n";
    for (int r = 0; r < 16; ++r)
    {
        std::cout << "  " << std::hex << std::uppercase << r << "_ ";
        for (int c = 0; c < 16; ++c)
        {
            uint8_t v = t[r * 16 + c];
            if (shiftDown) v >>= (8 - WIDTH);
            std::cout << "  " << int(v);
        }
        std::cout << "\n";
    }
    std::cout << std::dec;
}

// Вимірювання середнього часу обчислення CRC для одного повідомлення
volatile uint32_t g_sink = 0;   // щоб компілятор не викинув обчислення

template <class F>
double measureNs(const std::vector<std::vector<uint8_t>>& msgs, F&& f)
{
    using clock = std::chrono::steady_clock;
    uint32_t acc = 0;

    for (const auto& m : msgs) acc += f(m);   // прогрів

    auto t0 = clock::now();
    for (int r = 0; r < REPEATS; ++r)
        for (const auto& m : msgs) acc += f(m);
    auto t1 = clock::now();

    g_sink = g_sink + acc;
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return ns / (double(REPEATS) * double(msgs.size()));
}

// ---------------------------------------------------------------------------
int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::cout << "=====================================================================\n";
    std::cout << " Лабораторна робота №4. Алгоритми CRC. Варіант 12\n";
    std::cout << " CRC-4-ITU:  G(x) = x^4 + x + 1  ->  " << toBin(POLY_FULL, WIDTH + 1)
        << "b  (" << toHex(POLY_FULL) << ", без старшого біта " << toHex(POLY) << ")\n";
    std::cout << " Дзеркальний поліном: " << toBin(POLY_REFLECT, WIDTH) << "b (" << toHex(POLY_REFLECT) << ")\n";
    std::cout << "=====================================================================\n\n";

    // ---- Перевірка на прикладі з підручника (рис. 4.1) -----------------
    {
        std::vector<uint8_t> bits = { 1,1,0,1,0,1,1,0,1,1 };   // 1101011011
        auto bytes = packBits(bits);
        uint8_t c = crcSimple(bytes.data(), bits.size());
        std::cout << "Перевірка на прикладі рис. 4.1: повідомлення 1101011011, поліном 10011\n";
        std::cout << "  CRC (простий алгоритм) = " << toBin(c, WIDTH)
            << "   очікується 1110  -> " << (c == 0xE ? "OK" : "ПОМИЛКА") << "\n\n";
    }

    // ---- Побудова таблиць ----------------------------------------------
    auto tb0 = std::chrono::steady_clock::now();
    const Table directTable = makeDirectTable();
    const Table reflectedTable = makeReflectedTable();
    auto tb1 = std::chrono::steady_clock::now();
    double tableBuildUs = std::chrono::duration<double, std::micro>(tb1 - tb0).count();

    printTable(directTable, "Таблиця прямого алгоритму (значення CRC, hex):", true);
    std::cout << "\n";
    printTable(reflectedTable, "Таблиця дзеркального алгоритму (значення CRC, hex):", false);
    std::cout << "\n";

    // ---- Генерація повідомлення K = 1000 біт ----------------------------
    std::random_device rd;
    const uint32_t seed = rd();
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int> bitDist(0, 1);

    std::vector<uint8_t> msgBits(K_BITS);
    for (auto& b : msgBits) b = uint8_t(bitDist(gen));
    std::vector<uint8_t> msg = packBits(msgBits);

    std::cout << "Повідомлення: K = " << K_BITS << " біт (" << K_BYTES
        << " байт), seed = " << seed << "\n";
    std::cout << "Перші 64 біти:  ";
    for (size_t i = 0; i < 64; ++i) std::cout << int(msgBits[i]);
    std::cout << "...\n";
    std::cout << "У hex (125 байт):\n";
    for (size_t i = 0; i < msg.size(); ++i)
    {
        std::cout << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << int(msg[i]);
        std::cout << ((i % 32 == 31 || i + 1 == msg.size()) ? "\n" : " ");
    }
    std::cout << std::dec << std::setfill(' ') << "\n";

    // ---- Обчислення CRC усіма алгоритмами -------------------------------
    uint8_t c1 = crcSimple(msg.data(), K_BITS);
    uint8_t c2 = crcTable(msg.data(), K_BYTES, directTable);
    uint8_t c3 = crcReflectedSimple(msg.data(), K_BYTES);
    uint8_t c4 = crcReflectedTable(msg.data(), K_BYTES, reflectedTable);

    std::cout << "CRC повідомлення:\n";
    std::cout << "  1. Простий послідовний      : " << toBin(c1, WIDTH) << "  (" << toHex(c1) << ")\n";
    std::cout << "  2. Табличний                : " << toBin(c2, WIDTH) << "  (" << toHex(c2) << ")\n";
    std::cout << "  3. Дзеркальний послідовний  : " << toBin(c3, WIDTH) << "  (" << toHex(c3) << ")\n";
    std::cout << "  4. Дзеркальний табличний    : " << toBin(c4, WIDTH) << "  (" << toHex(c4) << ")\n\n";

    std::cout << "Перевірки:\n";
    std::cout << "  Простий = Табличний                         -> " << (c1 == c2 ? "OK" : "ПОМИЛКА") << "\n";
    std::cout << "  Дзеркальний послідовний = Дзерк. табличний  -> " << (c3 == c4 ? "OK" : "ПОМИЛКА") << "\n";

    // Дзеркальний CRC повідомлення = віддзеркалений прямий CRC повідомлення,
    // у якому біти кожного байта переставлені у зворотному порядку.
    {
        std::vector<uint8_t> rev(msg.size());
        for (size_t i = 0; i < msg.size(); ++i) rev[i] = uint8_t(reflect(msg[i], 8));
        uint8_t cr = uint8_t(reflect(crcSimple(rev.data(), K_BITS), WIDTH));
        std::cout << "  Дзеркальний = reflect(прямий CRC від байтів з оберненим порядком біт) -> "
            << (cr == c3 ? "OK" : "ПОМИЛКА") << "\n";
    }
    // Повідомлення T = M * x^4 + CRC ділиться на G(x) без залишку
    {
        std::vector<uint8_t> tBits = msgBits;
        for (int i = WIDTH - 1; i >= 0; --i) tBits.push_back((c1 >> i) & 1);
        auto tBytes = packBits(tBits);
        uint8_t rem = crcSimple(tBytes.data(), tBits.size());
        std::cout << "  CRC(M + CRC) = " << toBin(rem, WIDTH) << " (залишок 0) -> "
            << (rem == 0 ? "OK" : "ПОМИЛКА") << "\n";
        // одиночна похибка має бути виявлена
        tBits[123] ^= 1;
        tBytes = packBits(tBits);
        rem = crcSimple(tBytes.data(), tBits.size());
        std::cout << "  Інвертовано біт 123: залишок = " << toBin(rem, WIDTH)
            << " (ненульовий) -> " << (rem != 0 ? "похибку виявлено" : "НЕ виявлено") << "\n\n";
    }

    // ---- Серія експериментів --------------------------------------------
    std::cout << "Серія експериментів: " << EXPERIMENTS << " випадкових повідомлень по "
        << K_BITS << " біт, кожна серія повторюється " << REPEATS << " разів.\n";

    std::vector<std::vector<uint8_t>> msgs(EXPERIMENTS, std::vector<uint8_t>(K_BYTES));
    for (auto& m : msgs)
    {
        std::vector<uint8_t> bits(K_BITS);
        for (auto& b : bits) b = uint8_t(bitDist(gen));
        m = packBits(bits);
    }

    // Узгодженість результатів на всіх повідомленнях
    size_t mismatchDirect = 0, mismatchRefl = 0;
    for (const auto& m : msgs)
    {
        if (crcSimple(m.data(), K_BITS) != crcTable(m.data(), K_BYTES, directTable)) ++mismatchDirect;
        if (crcReflectedSimple(m.data(), K_BYTES) != crcReflectedTable(m.data(), K_BYTES, reflectedTable)) ++mismatchRefl;
    }
    std::cout << "  Розбіжностей прямий послідовний / табличний        : " << mismatchDirect << "\n";
    std::cout << "  Розбіжностей дзеркальний послідовний / табличний   : " << mismatchRefl << "\n\n";

    double t1 = measureNs(msgs, [](const std::vector<uint8_t>& m) {
        return crcSimple(m.data(), K_BITS); });
    double t2 = measureNs(msgs, [&](const std::vector<uint8_t>& m) {
        return crcTable(m.data(), K_BYTES, directTable); });
    double t3 = measureNs(msgs, [](const std::vector<uint8_t>& m) {
        return crcReflectedSimple(m.data(), K_BYTES); });
    double t4 = measureNs(msgs, [&](const std::vector<uint8_t>& m) {
        return crcReflectedTable(m.data(), K_BYTES, reflectedTable); });

    // ---- Порівняльна таблиця --------------------------------------------
    struct Row { const char* name; double ns; const char* iters; const char* mem; };
    Row rows[] = {
        { "1. Простий послідовний     ", t1, "K+N = 1004 біт ", "регістр 1 байт         " },
        { "2. Табличний               ", t2, "K/8 = 125 байт ", "регістр + таблиця 256 Б" },
        { "3. Дзеркальний послідовний ", t3, "K   = 1000 біт ", "регістр 1 байт         " },
        { "4. Дзеркальний табличний   ", t4, "K/8 = 125 байт ", "регістр + таблиця 256 Б" },
    };

    std::cout << "---------------------------------------------------------------------------------------------\n";
    std::cout << " Алгоритм                    | Ітерацій циклу  | Пам'ять                 | Сер. час, нс | Прискорення\n";
    std::cout << "---------------------------------------------------------------------------------------------\n";
    for (const auto& r : rows)
    {
        std::cout << " " << r.name << " | " << r.iters << " | " << r.mem << " | " << std::setw(12) << std::fixed << std::setprecision(1) << r.ns
            << " | " << std::setw(8) << std::setprecision(2) << (t1 / r.ns) << "x\n";
    }
    std::cout << "---------------------------------------------------------------------------------------------\n";
    std::cout << "  Прискорення - відносно простого послідовного алгоритму.\n";
    std::cout << "  Побудова обох таблиць (одноразово): " << std::setprecision(2) << tableBuildUs << " мкс\n";
    std::cout << "  Середній час на 1 біт: прост. " << std::setprecision(3) << t1 / K_BITS
        << " нс, табл. " << t2 / K_BITS << " нс, дзерк. посл. " << t3 / K_BITS
        << " нс, дзерк. табл. " << t4 / K_BITS << " нс\n\n";

    std::cout << "Ресурсна складність:\n";
    std::cout << "  Усі алгоритми мають час O(K). Послідовні виконують на кожен біт зсув, OR/AND\n";
    std::cout << "  та умовний XOR (~8 ітерацій на байт), пам'ять O(1).\n";
    std::cout << "  Табличні виконують на кожен байт один XOR та одне звернення до таблиці\n";
    std::cout << "  (1 ітерація на байт), додаткова пам'ять - таблиця 2^8 = 256 записів по 1 байту,\n";
    std::cout << "  яка будується один раз за 256 * 8 кроків.\n";

    return 0;
}