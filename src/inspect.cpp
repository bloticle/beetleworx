#include <file.h>
#include <image.h>
#include <fmt/format.h>
#include <fstream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 3) {
        fmt::print(stderr, "Usage: em2-inspect input.xex output-image.bin\n");
        return 2;
    }
    try {
        const auto bytes = LoadFile(argv[1]);
        if (bytes.size() < 24 || std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) != "XEX2")
            throw std::runtime_error("Input is not an XEX2 executable");
        auto image = Image::ParseImage(bytes.data(), bytes.size());
        if (!image.data || !image.size || image.sections.empty())
            throw std::runtime_error("XEX image could not be loaded");
        std::ofstream dump(argv[2], std::ios::binary);
        dump.write(reinterpret_cast<const char*>(image.data.get()), image.size);
        if (!dump) throw std::runtime_error("Could not write loaded image");
        fmt::print("{{\n  \"base\": {},\n  \"size\": {},\n  \"entry_point\": {},\n  \"sections\": [\n", image.base, image.size, image.entry_point);
        bool first = true;
        for (const auto& section : image.sections) {
            if (!first) fmt::print(",\n");
            first = false;
            fmt::print("    {{\"name\": \"{}\", \"base\": {}, \"size\": {}, \"flags\": {}}}", section.name.substr(0, 8), section.base, section.size, static_cast<unsigned>(section.flags));
        }
        fmt::print("\n  ],\n  \"imports\": [\n");
        first = true;
        for (const auto& symbol : image.symbols) {
            if (!first) fmt::print(",\n");
            first = false;
            fmt::print("    {{\"name\": \"{}\", \"address\": {}}}", symbol.name, symbol.address);
        }
        fmt::print("\n  ]\n}}\n");
    } catch (const std::exception& e) {
        fmt::print(stderr, "em2-inspect: {}\n", e.what());
        return 1;
    }
}
