// mod_bake: build N64ModernRuntime mods (.nrm) into an executable, for targets
// that cannot generate or patch code at runtime (the runtime's
// RECOMP_STATIC_MODS mode).
//
//   mod_bake inspect <mod_syms.bin> <mod_binary.bin> <game syms.toml>
//   mod_bake bake <game syms.toml> <patches.elf> <out dir> <mod.nrm>[:embed]...
//
// inspect: what the mod needs from a static runtime: its sections, the game
// functions it replaces and hooks, its dependencies, imports, exports, events
// and callbacks.
//
// bake writes into <out dir>:
//   mod_<id>.c         the mod's code as C, private to the file except for one
//                      recomp_static_mod descriptor
//   static_dispatch.c  the base functions baked mods replace or hook
//   static_mods.cpp    recomp_register_static_mods(): registers the above and
//                      the .nrm files marked :embed (built into the executable,
//                      without thumbnails, and disabled until turned on)
//   static_mods_data.c those .nrm files' bytes
//   game_hooks.toml    [[patches.hook]] entries for the game's recompilation,
//   patch_hooks.toml   and for the patches' (functions they replace): a check
//                      of the function's dispatch slot at its entry
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "recompiler/context.h"
#include "rabbitizer.hpp"
#include "elfio/elfio.hpp"
#include "json.hpp"
#include "miniz.h"
#include "miniz_zip.h"

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream f{ path, std::ios::binary };
    if (!f) { fprintf(stderr, "mod_bake: cannot open %s\n", path.string().c_str()); exit(2); }
    return std::vector<uint8_t>{ std::istreambuf_iterator<char>(f), {} };
}

bool load_reference(const fs::path& game_syms, N64Recomp::Context& ref) {
    std::vector<uint8_t> dummy_rom;
    if (!N64Recomp::Context::from_symbol_file(game_syms, std::move(dummy_rom), ref, false)) {
        fprintf(stderr, "mod_bake: cannot load %s\n", game_syms.string().c_str());
        return false;
    }
    return true;
}

// Parses a mod against the game's symbols, as the runtime does, and resolves
// its calls into the game (R_MIPS_26 reference relocs) to reference symbols.
bool load_mod(std::span<const uint8_t> syms, std::span<const uint8_t> binary, const N64Recomp::Context& ref, N64Recomp::Context& mod) {
    std::unordered_map<uint32_t, uint16_t> sections_by_vrom;
    for (uint16_t i = 0; i < ref.sections.size(); i++) sections_by_vrom[ref.sections[i].rom_addr] = i;
    std::span<const char> syms_span{ reinterpret_cast<const char*>(syms.data()), syms.size() };
    N64Recomp::ModSymbolsError err = N64Recomp::parse_mod_symbols(syms_span, binary, sections_by_vrom, mod);
    if (err != N64Recomp::ModSymbolsError::Good) {
        fprintf(stderr, "mod_bake: parse_mod_symbols: error %d\n", (int)err);
        return false;
    }
    mod.import_reference_context(ref);

    std::unordered_map<uint32_t, std::vector<size_t>> reference_symbols_by_vram;
    for (size_t i = 0; i < mod.num_regular_reference_symbols(); i++) {
        const auto& sym = mod.get_regular_reference_symbol(i);
        if (sym.section_index != N64Recomp::SectionAbsolute) {
            reference_symbols_by_vram[mod.get_reference_section_vram(sym.section_index) + sym.section_offset].push_back(i);
        }
    }
    for (auto& section : mod.sections) {
        for (auto& reloc : section.relocs) {
            if (reloc.type != N64Recomp::RelocType::R_MIPS_26 || !reloc.reference_symbol || !mod.is_regular_reference_section(reloc.target_section)) continue;
            uint32_t target = mod.get_reference_section_vram(reloc.target_section) + reloc.target_section_offset;
            bool found = false;
            auto it = reference_symbols_by_vram.find(target);
            if (it != reference_symbols_by_vram.end()) {
                for (size_t i : it->second) {
                    if (mod.get_reference_symbol(reloc.target_section, i).section_index == reloc.target_section) {
                        reloc.symbol_index = i;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                fprintf(stderr, "mod_bake: no game function at 0x%08X (section %u) for a call in the mod\n", target, reloc.target_section);
                return false;
            }
        }
    }
    mod.rom.assign(binary.begin(), binary.end());
    return true;
}

// Name of the game function at (section rom, vram), as the recompilations know it.
std::string game_function_name(const N64Recomp::Context& ref, uint32_t section_rom, uint32_t vram) {
    for (size_t s = 0; s < ref.sections.size(); s++) {
        if (ref.sections[s].rom_addr != section_rom) continue;
        for (const auto& f : ref.functions) {
            if (f.section_index == s && f.vram == vram) return f.name;
        }
    }
    return {};
}

int inspect(const char* syms_path, const char* binary_path, const char* game_syms) {
    N64Recomp::Context ref, m;
    if (!load_reference(game_syms, ref)) return 1;
    std::vector<uint8_t> syms = read_file(syms_path), binary = read_file(binary_path);
    if (!load_mod(syms, binary, ref, m)) return 1;
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
        printf("  %s%s\n", game_function_name(ref, r.original_section_vrom, r.original_vram).c_str(),
               (r.flags & N64Recomp::ReplacementFlags::Force) == N64Recomp::ReplacementFlags::Force ? " (force)" : "");
    printf("hooks %zu\n", m.hooks.size());
    for (const auto& h : m.hooks)
        printf("  %s at %s\n", game_function_name(ref, h.original_section_vrom, h.original_vram).c_str(),
               (h.flags & N64Recomp::HookFlags::AtReturn) == N64Recomp::HookFlags::AtReturn ? "return" : "entry");
    printf("dependencies %zu:", m.dependencies.size());
    for (const auto& d : m.dependencies) printf(" %s", d.c_str());
    printf("\nimports %zu\n", m.import_symbols.size());
    for (const auto& s : m.import_symbols) printf("  %s:%s\n", m.dependencies[s.dependency_index].c_str(), s.base.name.c_str());
    printf("dependency events %zu\n", m.dependency_events.size());
    for (const auto& e : m.dependency_events) printf("  %s:%s\n", m.dependencies[e.dependency_index].c_str(), e.event_name.c_str());
    printf("callbacks %zu\n", m.callbacks.size());
    printf("exports %zu:", m.exported_funcs.size());
    for (size_t fi : m.exported_funcs) printf(" %s", m.functions[fi].name.c_str());
    printf("\nevents %zu:", m.event_symbols.size());
    for (const auto& e : m.event_symbols) printf(" %s", e.base.name.c_str());
    printf("\n");
    return 0;
}

// ---------------------------------------------------------------- bake

struct Nrm {
    fs::path path;
    bool embed = false;
    std::string id, version;
    std::vector<uint8_t> manifest, syms, binary;
    std::string ident;      // C identifier derived from the id
};

bool read_zip_file(mz_zip_archive& zip, const char* name, std::vector<uint8_t>& out) {
    size_t size = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&zip, name, &size, 0);
    if (p == nullptr) return false;
    out.assign((uint8_t*)p, (uint8_t*)p + size);
    mz_free(p);
    return true;
}

bool read_nrm(Nrm& n) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, n.path.string().c_str(), 0)) {
        fprintf(stderr, "mod_bake: %s is not a zip\n", n.path.string().c_str());
        return false;
    }
    std::vector<uint8_t>& manifest = n.manifest;
    bool ok = read_zip_file(zip, "mod.json", manifest) && read_zip_file(zip, "mod_syms.bin", n.syms) && read_zip_file(zip, "mod_binary.bin", n.binary);
    mz_zip_reader_end(&zip);
    if (!ok) {
        fprintf(stderr, "mod_bake: %s lacks mod.json, mod_syms.bin or mod_binary.bin\n", n.path.string().c_str());
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(manifest.begin(), manifest.end());
        n.id = j.at("id").get<std::string>();
        n.version = j.value("version", std::string{});
    }
    catch (const std::exception& e) {
        fprintf(stderr, "mod_bake: %s: mod.json: %s\n", n.path.string().c_str(), e.what());
        return false;
    }
    for (char c : n.id) n.ident += (isalnum((unsigned char)c) ? c : '_');
    return true;
}

// The .nrm reduced to what the runtime reads (no thumbnail), for embedding.
// A mod built into the executable starts disabled: the player turns it on.
std::vector<uint8_t> stripped_nrm(const Nrm& n) {
    mz_zip_archive zip{};
    std::vector<uint8_t> out;
    nlohmann::json j = nlohmann::json::parse(n.manifest.begin(), n.manifest.end());
    j["enabled_by_default"] = false;
    const std::string manifest = j.dump(4);
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return out;
    bool ok = mz_zip_writer_add_mem(&zip, "mod.json", manifest.data(), manifest.size(), MZ_BEST_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, "mod_syms.bin", n.syms.data(), n.syms.size(), MZ_BEST_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, "mod_binary.bin", n.binary.data(), n.binary.size(), MZ_BEST_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    if (ok && mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
        out.assign((uint8_t*)buf, (uint8_t*)buf + size);
        mz_free(buf);   // the caller's once finalized
    }
    mz_zip_writer_end(&zip);
    return out;
}

uint64_t fnv1a64(const std::vector<uint8_t>& d) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint8_t b : d) { h ^= b; h *= 0x100000001B3ull; }
    return h;
}

// Function symbols defined in the patches' ELF: game functions the patches replace.
bool read_patch_functions(const fs::path& elf_path, std::unordered_set<std::string>& out) {
    ELFIO::elfio elf;
    if (!elf.load(elf_path.string())) {
        fprintf(stderr, "mod_bake: cannot read %s\n", elf_path.string().c_str());
        return false;
    }
    for (const auto& sec : elf.sections) {
        if (sec->get_type() != ELFIO::SHT_SYMTAB) continue;
        ELFIO::const_symbol_section_accessor syms(elf, sec.get());
        for (ELFIO::Elf_Xword i = 0; i < syms.get_symbols_num(); i++) {
            std::string name;
            ELFIO::Elf64_Addr value; ELFIO::Elf_Xword size;
            unsigned char bind, type, other; ELFIO::Elf_Half shndx;
            syms.get_symbol(i, name, value, size, bind, type, shndx, other);
            if (type == ELFIO::STT_FUNC && shndx != ELFIO::SHN_UNDEF && !name.empty()) out.insert(name);
        }
    }
    return true;
}

// The mod's code as C. Everything is static except the descriptor.
bool write_mod_c(const Nrm& n, N64Recomp::Context& mod, const fs::path& out_path) {
    std::ostringstream o;
    const size_t num_sections = mod.sections.size();
    const size_t num_imports = mod.import_symbols.size();

    o << "// Generated by mod_bake from " << n.path.filename().string() << " (" << n.id << " " << n.version << "). Do not edit.\n"
      << "#include \"recomp.h\"\n"
      << "#include \"librecomp/static_mods.h\"\n\n"
      << "// The mod's functions are private to this file.\n"
      << "#undef RECOMP_FUNC\n"
      << "#if defined(__clang__)\n#define RECOMP_FUNC static __attribute__((noinline))\n"
      << "#elif defined(__GNUC__)\n#define RECOMP_FUNC static __attribute__((noipa, optimize(\"rounding-math\")))\n"
      << "#else\n#define RECOMP_FUNC static\n#endif\n\n"
      << "// Where the runtime placed the mod's sections; the game's are the runtime's section_addresses.\n"
      << "static int32_t mod_section_addresses[" << std::max<size_t>(1, num_sections) << "];\n"
      << "#undef RELOC_HI16\n#undef RELOC_LO16\n"
      << "#define RELOC_HI16(section_index, offset) HI16(mod_section_addresses[section_index] + (offset))\n"
      << "#define RELOC_LO16(section_index, offset) LO16(mod_section_addresses[section_index] + (offset))\n"
      << "#define REF_RELOC_HI16(section_index, offset) HI16(section_addresses[section_index] + (offset))\n"
      << "#define REF_RELOC_LO16(section_index, offset) LO16(section_addresses[section_index] + (offset))\n\n"
      << "static uint32_t base_event_index;\n\n";

    // Imports: called through slots the runtime fills. A shim per import
    // serves base exports that take an extra argument.
    o << "// Imports, filled by the runtime.\n"
      << "static recomp_func_t* imported_funcs[" << std::max<size_t>(1, num_imports) << "];\n"
      << "static recomp_static_mod_ext_import ext_imports[" << std::max<size_t>(1, num_imports) << "];\n";
    for (size_t i = 0; i < num_imports; i++) {
        o << "static void ext_shim_" << i << "(uint8_t* rdram, recomp_context* ctx) { ext_imports[" << i << "].func(rdram, ctx, ext_imports[" << i << "].arg); }\n";
    }
    o << "static recomp_func_t* const ext_import_shims[" << std::max<size_t>(1, num_imports) << "] = {";
    for (size_t i = 0; i < num_imports; i++) o << (i ? ", " : " ") << "ext_shim_" << i;
    if (num_imports == 0) o << " 0";
    o << " };\n";
    std::set<std::string> defined;
    for (size_t i = 0; i < num_imports; i++) {
        const std::string& name = mod.import_symbols[i].base.name;
        if (defined.insert(name).second) o << "#define " << name << " imported_funcs[" << i << "]\n";
    }
    o << "\n";

    // Calls into the game: one slot per call site, as the runtime fills them.
    o << "// Game functions the mod calls, filled by the runtime (one slot per call).\n";
    size_t num_reference = 0;
    std::ostringstream defines;
    for (const auto& section : mod.sections) {
        for (const auto& reloc : section.relocs) {
            if (reloc.type == N64Recomp::RelocType::R_MIPS_26 && reloc.reference_symbol && mod.is_regular_reference_section(reloc.target_section)) {
                const auto& sym = mod.get_reference_symbol(reloc.target_section, reloc.symbol_index);
                if (defined.insert(sym.name).second) defines << "#define " << sym.name << " reference_symbol_funcs[" << num_reference << "]\n";
                num_reference++;
            }
        }
    }
    o << "static recomp_func_t* reference_symbol_funcs[" << std::max<size_t>(1, num_reference) << "];\n" << defines.str() << "\n";

    // Functions.
    for (size_t i = 0; i < mod.functions.size(); i++) mod.functions[i].name = "mod_func_" + std::to_string(i);
    o << "// Functions.\n";
    for (const auto& f : mod.functions) o << "RECOMP_FUNC void " << f.name << "(uint8_t* rdram, recomp_context* ctx);\n";
    o << "\n";
    std::vector<std::vector<uint32_t>> static_funcs_by_section(mod.sections.size());
    RabbitizerConfig_Cfg.pseudos.pseudoMove = false;
    RabbitizerConfig_Cfg.pseudos.pseudoBeqz = false;
    RabbitizerConfig_Cfg.pseudos.pseudoBnez = false;
    RabbitizerConfig_Cfg.pseudos.pseudoNot = false;
    RabbitizerConfig_Cfg.pseudos.pseudoBal = false;
    for (size_t i = 0; i < mod.functions.size(); i++) {
        if (!N64Recomp::recompile_function(mod, i, o, static_funcs_by_section, true)) {
            fprintf(stderr, "mod_bake: %s: failed to recompile function %zu\n", n.id.c_str(), i);
            return false;
        }
    }

    // The descriptor.
    o << "\nstatic recomp_func_t* const functions[" << std::max<size_t>(1, mod.functions.size()) << "] = {";
    for (size_t i = 0; i < mod.functions.size(); i++) o << (i % 8 == 0 ? "\n    " : " ") << mod.functions[i].name << ",";
    o << "\n};\n\n"
      << "const recomp_static_mod static_mod_" << n.ident << " = {\n"
      << "    \"" << n.id << "\", 0x" << std::hex << fnv1a64(n.binary) << std::dec << "ull,\n"
      << "    " << mod.functions.size() << ", functions,\n"
      << "    " << num_imports << ", imported_funcs, ext_import_shims, ext_imports,\n"
      << "    " << num_reference << ", reference_symbol_funcs,\n"
      << "    &base_event_index,\n"
      << "    " << num_sections << ", mod_section_addresses,\n"
      << "};\n";

    std::ofstream f{ out_path };
    f << o.str();
    return f.good();
}

// A base function a baked mod replaces or hooks.
struct DispatchTarget {
    uint32_t section_rom, vram;
    std::string name;
    bool operator<(const DispatchTarget& r) const { return section_rom != r.section_rom ? section_rom < r.section_rom : vram < r.vram; }
};

std::string hook_text(size_t slot) {
    // Block-scope declarations: the recompiled files need no header for it.
    return "{ extern unsigned char recomp_static_dispatch_active[]; extern int recomp_static_dispatch(uint8_t*, recomp_context*, uint32_t); "
           "if (recomp_static_dispatch_active[" + std::to_string(slot) + "] && recomp_static_dispatch(rdram, ctx, " + std::to_string(slot) + ")) return; }";
}

int bake(int argc, char** argv) {
    const fs::path game_syms = argv[2], patches_elf = argv[3], out_dir = argv[4];
    N64Recomp::Context ref;
    if (!load_reference(game_syms, ref)) return 1;
    std::unordered_set<std::string> patch_funcs;
    if (!read_patch_functions(patches_elf, patch_funcs)) return 1;
    fs::create_directories(out_dir);

    std::vector<Nrm> mods;
    std::set<DispatchTarget> targets;
    std::set<std::string> ids;
    for (int i = 5; i < argc; i++) {
        Nrm n;
        std::string arg = argv[i];
        if (arg.size() > 6 && arg.ends_with(":embed")) { n.embed = true; arg.resize(arg.size() - 6); }
        n.path = arg;
        if (!read_nrm(n)) return 1;
        if (!ids.insert(n.ident).second) { fprintf(stderr, "mod_bake: mod %s given twice\n", n.id.c_str()); return 1; }

        N64Recomp::Context mod;
        if (!load_mod(n.syms, n.binary, ref, mod)) { fprintf(stderr, "mod_bake: in %s\n", n.path.string().c_str()); return 1; }
        auto add_target = [&](uint32_t rom, uint32_t vram, const char* what) {
            std::string name = game_function_name(ref, rom, vram);
            if (name.empty()) {
                fprintf(stderr, "mod_bake: %s %s a function the game's symbols lack (section 0x%X vram 0x%08X)\n", n.id.c_str(), what, rom, vram);
                return false;
            }
            targets.insert(DispatchTarget{ rom, vram, name });
            return true;
        };
        for (const auto& r : mod.replacements) if (!add_target(r.original_section_vrom, r.original_vram, "replaces")) return 1;
        for (const auto& h : mod.hooks) if (!add_target(h.original_section_vrom, h.original_vram, "hooks")) return 1;
        if (!write_mod_c(n, mod, out_dir / ("mod_" + n.ident + ".c"))) return 1;
        printf("mod_bake: %s %s: %zu functions, %zu replacements, %zu hooks, %zu exports, %zu events%s\n", n.id.c_str(), n.version.c_str(),
               mod.functions.size(), mod.replacements.size(), mod.hooks.size(), mod.exported_funcs.size(), mod.event_symbols.size(),
               n.embed ? " (embedded)" : "");
        mods.push_back(std::move(n));
    }

    // Dispatch table and the recompiler hooks for it.
    std::vector<DispatchTarget> slots(targets.begin(), targets.end());
    {
        std::ofstream o{ out_dir / "static_dispatch.c" };
        o << "// Generated by mod_bake. Do not edit.\n#include \"librecomp/static_mods.h\"\n\n"
          << "// Base functions baked mods replace or hook, by dispatch slot.\n"
          << "const recomp_static_dispatch_entry recomp_static_dispatch_entries[" << std::max<size_t>(1, slots.size()) << "] = {\n";
        for (const auto& t : slots) o << "    { 0x" << std::hex << t.section_rom << ", 0x" << t.vram << std::dec << " },   // " << t.name << "\n";
        if (slots.empty()) o << "    { 0, 0 },\n";
        o << "};\nconst uint32_t recomp_static_dispatch_count = " << slots.size() << ";\n"
          << "unsigned char recomp_static_dispatch_active[" << std::max<size_t>(1, slots.size()) << "];\n";
    }
    std::ofstream game_hooks{ out_dir / "game_hooks.toml" }, patch_hooks{ out_dir / "patch_hooks.toml" };
    game_hooks << "# Generated by mod_bake: the dispatch check at the entry of game functions baked mods replace or hook.\n";
    patch_hooks << "# Generated by mod_bake: the same for the patches' versions of those functions.\n";
    for (size_t i = 0; i < slots.size(); i++) {
        // A TOML literal string: the text has no single quotes.
        std::string entry = "\n[[patches.hook]]\nfunc = \"" + slots[i].name + "\"\ntext = '" + hook_text(i) + "'\n";
        game_hooks << entry;
        if (patch_funcs.contains(slots[i].name)) patch_hooks << entry;
    }

    // Registration, and the embedded .nrm files.
    {
        std::ofstream o{ out_dir / "static_mods_data.c" };
        o << "// Generated by mod_bake: the .nrm files built into the executable. Do not edit.\n#include <stddef.h>\n\n";
        for (const auto& n : mods) {
            if (!n.embed) continue;
            std::vector<uint8_t> bytes = stripped_nrm(n);
            if (bytes.empty()) { fprintf(stderr, "mod_bake: cannot repack %s\n", n.id.c_str()); return 1; }
            o << "const unsigned char static_mod_nrm_" << n.ident << "[" << bytes.size() << "] = {";
            for (size_t i = 0; i < bytes.size(); i++) o << (i % 24 == 0 ? "\n" : "") << (unsigned)bytes[i] << ",";
            o << "\n};\nconst size_t static_mod_nrm_" << n.ident << "_size = " << bytes.size() << ";\n";
        }
    }
    {
        std::ofstream o{ out_dir / "static_mods.cpp" };
        o << "// Generated by mod_bake. Do not edit.\n#include <span>\n#include \"librecomp/mods.hpp\"\n#include \"librecomp/static_mods.h\"\n\n"
          << "extern \"C\" {\n"
          << "extern const recomp_static_dispatch_entry recomp_static_dispatch_entries[];\n"
          << "extern const uint32_t recomp_static_dispatch_count;\n"
          << "extern unsigned char recomp_static_dispatch_active[];\n";
        for (const auto& n : mods) {
            o << "extern const recomp_static_mod static_mod_" << n.ident << ";\n";
            if (n.embed) o << "extern const unsigned char static_mod_nrm_" << n.ident << "[];\nextern const size_t static_mod_nrm_" << n.ident << "_size;\n";
        }
        o << "}\n\n// The baked mods, before recomp::start.\nvoid recomp_register_static_mods() {\n"
          << "    recomp::mods::register_static_dispatch(recomp_static_dispatch_entries, recomp_static_dispatch_count, recomp_static_dispatch_active);\n";
        for (const auto& n : mods) {
            o << "    recomp::mods::register_static_mod(&static_mod_" << n.ident << ");\n";
            if (n.embed) {
                o << "    recomp::mods::register_embedded_mod(\"" << n.id << "\", std::span<const uint8_t>{ static_mod_nrm_" << n.ident
                  << ", static_mod_nrm_" << n.ident << "_size });\n";
            }
        }
        o << "}\n";
    }
    printf("mod_bake: %zu mods, %zu dispatch slots (%zu in the patches)\n", mods.size(), slots.size(),
           (size_t)std::count_if(slots.begin(), slots.end(), [&](const DispatchTarget& t) { return patch_funcs.contains(t.name); }));
    return 0;
}

}   // namespace

int main(int argc, char** argv) {
    if (argc == 5 && strcmp(argv[1], "inspect") == 0) return inspect(argv[2], argv[3], argv[4]);
    if (argc >= 5 && strcmp(argv[1], "bake") == 0) return bake(argc, argv);
    fprintf(stderr, "usage: mod_bake inspect <mod_syms.bin> <mod_binary.bin> <game syms.toml>\n"
                    "       mod_bake bake <game syms.toml> <patches.elf> <out dir> <mod.nrm>[:embed]...\n");
    return 2;
}
