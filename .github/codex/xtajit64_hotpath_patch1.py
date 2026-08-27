from pathlib import Path

path = Path("dlls/xtajit64/unixlib.c")
source = path.read_text(encoding="utf-8")

def replace_once(old, new):
    global source
    count = source.count(old)
    if count != 1:
        raise SystemExit(f"expected one replacement, found {count}: {old[:80]!r}")
    source = source.replace(old, new, 1)

replace_once("    uint64_t ec_page_size;\n", "    uint64_t ec_page_shift;\n")
replace_once('static BOOL is_ec_code( uint64_t address )\n{\n    uint64_t page, word;\n\n    if (!provider.ec_bitmap || address > provider.highest_user_address) return FALSE;\n    page = address / provider.ec_page_size;\n    word = provider.ec_bitmap[page / 64];\n    return (word >> (page & 63)) & 1;\n}\n', 'static inline BOOL is_ec_code( uint64_t address )\n{\n    uint64_t page;\n\n    if (!provider.ec_bitmap || address > provider.highest_user_address) return FALSE;\n    page = address >> provider.ec_page_shift;\n    return (provider.ec_bitmap[page >> 6] >> (page & 63)) & 1;\n}\n')
replace_once(
    "    provider.ec_page_size = params->kuser_size;\n",
    "    provider.ec_page_shift = __builtin_ctzll( params->kuser_size );\n",
)
replace_once("    provider.ec_page_size = 0;\n", "    provider.ec_page_shift = 0;\n")
path.write_text(source, encoding="utf-8")
