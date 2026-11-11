#include <function.h>
#include <byteswap.h>
#include <array>
#include <iostream>
#include <stdexcept>

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        // An instruction after the supplied region must never be read or followed.
        std::array<uint32_t, 2> tail{ByteSwap(0x48001000u), ByteSwap(0x60000000u)};
        check(Function::Analyze(tail.data(), 4, 0x82000000).size == 4, "Forward tail branch escaped region");
        std::array<uint32_t, 3> loop{ByteSwap(0x4200FFFCu), ByteSwap(0x4D9A0020u), ByteSwap(0x4E800020u)};
        check(Function::Analyze(loop.data(), 12, 0x82000000).size == 12, "Conditional return or backward branch analysis failed");
        std::array<uint32_t, 4> counter_returns{ByteSwap(0x4F400020u), ByteSwap(0x98860001u), ByteSwap(0x4F400020u), ByteSwap(0x4E800020u)};
        check(Function::Analyze(counter_returns.data(), 16, 0x82000000).size == 16, "bdzlr truncated a fallthrough path");
        counter_returns[0] = ByteSwap(0x4E000020u);
        check(Function::Analyze(counter_returns.data(), 16, 0x82000000).size == 16, "bdnzlr truncated a fallthrough path");
        std::array<uint32_t, 5> table{ByteSwap(0x4E800420u), 0, 0, ByteSwap(0x38600001u), ByteSwap(0x4E800020u)};
        std::unordered_map<size_t, std::vector<size_t>> targets{{0x82000000, {0x8200000C}}};
        check(Function::Analyze(table.data(), 20, 0x82000000, &targets).size == 20, "Inline switch cases were truncated");
        check(Function::Analyze(table.data(), 20, 0x82000000).size == 4, "Indirect call followed unknown targets");
        std::cout << "Function boundary regressions passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
