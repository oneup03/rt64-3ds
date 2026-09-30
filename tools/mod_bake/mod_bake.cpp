// mod_bake: host tool for building N64ModernRuntime mods into a 3DS
// executable (no runtime code generation there).
//
//   mod_bake inspect <mod_syms.bin> <mod_binary.bin> <game syms.toml>
//
// inspect: what the mod needs from a static runtime: its sections, the
// game functions it replaces and hooks, its dependencies, imports, exports,
// events and callbacks.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "recompiler/context.h"

namespace {

std::vector<uint8_t> read_file(const char* path) {
    std::ifstream f{ path, std::ios::binary };
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    return std::vector<uint8_t>{ std::istreambuf_iterator<char>(f), {} };
}

struct Loaded {
    N64Recomp::Context ref;
    N64Recomp::Context mod;
    std::map<uint32_t, std::string> ref_names;   // vram -> game function name
};

bool load(const char* syms, const char* binary, const char* game_syms, Loaded& out) {
    std::vector<uint8_t> symbol_data = read_file(syms);
    std::vector<uint8_t> binary_data = read_file(binary);
    std::vector<uint8_t> dummy_rom;
    if (!N64Recomp::Context::from_symbol_file(game_syms, std::move(dummy_rom), out.ref, false)) {
        fprintf(stderr, "cannot load %s\n", game_syms);
        return false;
    }
    std::unordered_map<uint32_t, uint16_t> sections_by_vrom;
    for (uint16_t i = 0; i < out.ref.sections.size(); i++) sections_by_vrom[out.ref.sections[i].rom_addr] = i;
    std::span<const char> span{ reinterpret_cast<const char*>(symbol_data.data()), symbol_data.size() };
    N64Recomp::ModSymbolsError err = N64Recomp::parse_mod_symbols(span, binary_data, sections_by_vrom, out.mod);
    if (err != N64Recomp::ModSymbolsError::Good) {
        fprintf(stderr, "parse_mod_symbols: error %d\n", (int)err);
        return false;
    }
    out.mod.import_reference_context(out.ref);
    for (const auto& f : out.ref.functions) out.ref_names[f.vram] = f.name;
    return true;
}

const char* game_name(const Loaded& l, uint32_t vram) {
    auto it = l.ref_names.find(vram);
    return it != l.ref_names.end() ? it->second.c_str() : "?";
}

int inspect(const Loaded& l) {
    const N64Recomp::Context& m = l.mod;
    size_t code_bytes = 0;
    for (const auto& f : m.functions) code_bytes += f.words.size() * 4;
    printf("sections %zu, functions %zu (%zu bytes of MIPS code)\n", m.sections.size(), m.functions.size(), code_bytes);
    for (size_t i = 0; i < m.sections.size(); i++) {
        const auto& s = m.sections[i];
        printf("  section %zu %-24s vram %08x size %6x bss %6x relocs %5zu%s%s%s%s\n", i, s.name.c_str(), s.ram_addr, s.size, s.bss_size,
               s.relocs.size(), s.executable ? " exec" : "", s.relocatable ? " relocatable" : "", s.fixed_address ? " fixed" : "",
               s.globally_loaded ? " global" : "");
    }
    printf("replacements %zu\n", m.replacements.size());
    for (const auto& r : m.replacements)
        printf("  %-40s <- %s%s\n", game_name(l, r.original_vram), m.functions[r.func_index].name.c_str(),
               (r.flags & N64Recomp::ReplacementFlags::Force) == N64Recomp::ReplacementFlags::Force ? " (force)" : "");
    printf("hooks %zu\n", m.hooks.size());
    for (const auto& h : m.hooks)
        printf("  %-40s %s %s\n", game_name(l, h.original_vram),
               (h.flags & N64Recomp::HookFlags::AtReturn) == N64Recomp::HookFlags::AtReturn ? "return" : "entry ", m.functions[h.func_index].name.c_str());
    printf("dependencies %zu:", m.dependencies.size());
    for (const auto& d : m.dependencies) printf(" %s", d.c_str());
    printf("\nimports %zu\n", m.import_symbols.size());
    for (const auto& s : m.import_symbols) printf("  %s:%s\n", m.dependencies[s.dependency_index].c_str(), s.base.name.c_str());
    printf("dependency events %zu\n", m.dependency_events.size());
    for (const auto& e : m.dependency_events) printf("  %s:%s\n", m.dependencies[e.dependency_index].c_str(), e.event_name.c_str());
    printf("callbacks %zu\n", m.callbacks.size());
    for (const auto& c : m.callbacks) {
        const auto& e = m.dependency_events[c.dependency_event_index];
        printf("  %s:%s -> %s\n", m.dependencies[e.dependency_index].c_str(), e.event_name.c_str(), m.functions[c.function_index].name.c_str());
    }
    printf("exports %zu:", m.exported_funcs.size());
    for (size_t fi : m.exported_funcs) printf(" %s", m.functions[fi].name.c_str());
    printf("\nevents %zu:", m.event_symbols.size());
    for (const auto& e : m.event_symbols) printf(" %s", e.base.name.c_str());
    printf("\n");
    return 0;
}

}   // namespace

int main(int argc, char** argv) {
    if (argc == 5 && strcmp(argv[1], "inspect") == 0) {
        Loaded l;
        if (!load(argv[2], argv[3], argv[4], l)) return 1;
        return inspect(l);
    }
    fprintf(stderr, "usage: mod_bake inspect <mod_syms.bin> <mod_binary.bin> <game syms.toml>\n");
    return 2;
}
