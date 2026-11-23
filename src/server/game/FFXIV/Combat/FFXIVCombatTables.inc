// GENERATED FILE -- do not edit.
// Generator: work/stat-research/gen_ffxiv_combat_tables.py
// Inputs:    data/ffxiv-combat-constants-20261123.tsv (f(AP) anchors),
//            B-FORMULA-REFERENCE F23.3/F23.4/F23.7/F23.13/F24.2, I-REFRESH 9.3/9.5
//
// Every row carries its own provenance bit; nothing below is an invented
// coefficient. Anchors are verbatim from the cited source; intermediate
// levels are linear (truncated) and flagged INTERPOLATED; levels with no
// data hold the nearest anchor and are flagged HELD.

namespace FFXIV::Combat::Tables
{
// level, f(AP) coefficient for non-tanks, for tanks, provenance
constexpr FfxivAttackPowerRow kAttackPowerTable[FFXIV_COMBAT_LEVEL_COUNT] =
{
    {   1,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {   2,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {   3,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {   4,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {   5,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {   6,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {   7,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {   8,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {   9,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  10,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  11,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  12,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  13,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  14,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  15,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  16,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  17,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  18,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  19,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  20,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  21,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  22,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  23,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  24,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  25,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  26,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  27,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  28,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  29,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  30,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  31,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  32,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  33,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  34,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  35,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  36,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  37,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  38,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  39,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  40,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  41,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  42,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  43,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  44,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  45,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  46,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  47,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  48,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  49,   75,   56, 0x2, 0x8 }, // other published Lv1-50 flat band / tank held, no tank value below 50
    {  50,   75,   56, 0x1, 0x1 }, // anchor
    {  51,   78,   59, 0x4, 0x4 }, // interpolated
    {  52,   82,   63, 0x4, 0x4 }, // interpolated
    {  53,   86,   66, 0x4, 0x4 }, // interpolated
    {  54,   90,   70, 0x4, 0x4 }, // interpolated
    {  55,   94,   73, 0x4, 0x4 }, // interpolated
    {  56,   98,   77, 0x4, 0x4 }, // interpolated
    {  57,  102,   80, 0x4, 0x4 }, // interpolated
    {  58,  106,   84, 0x4, 0x4 }, // interpolated
    {  59,  110,   87, 0x4, 0x4 }, // interpolated
    {  60,  114,   91, 0x1, 0x1 }, // anchor
    {  61,  115,   92, 0x4, 0x4 }, // interpolated
    {  62,  116,   93, 0x4, 0x4 }, // interpolated
    {  63,  117,   95, 0x4, 0x4 }, // interpolated
    {  64,  118,   96, 0x4, 0x4 }, // interpolated
    {  65,  119,   98, 0x4, 0x4 }, // interpolated
    {  66,  120,   99, 0x4, 0x4 }, // interpolated
    {  67,  121,  100, 0x4, 0x4 }, // interpolated
    {  68,  122,  102, 0x4, 0x4 }, // interpolated
    {  69,  123,  103, 0x4, 0x4 }, // interpolated
    {  70,  125,  105, 0x1, 0x1 }, // anchor
    {  71,  129,  106, 0x4, 0x4 }, // interpolated
    {  72,  133,  107, 0x4, 0x4 }, // interpolated
    {  73,  137,  108, 0x4, 0x4 }, // interpolated
    {  74,  141,  109, 0x4, 0x4 }, // interpolated
    {  75,  145,  110, 0x4, 0x4 }, // interpolated
    {  76,  149,  111, 0x4, 0x4 }, // interpolated
    {  77,  153,  112, 0x4, 0x4 }, // interpolated
    {  78,  157,  113, 0x4, 0x4 }, // interpolated
    {  79,  161,  114, 0x4, 0x4 }, // interpolated
    {  80,  165,  115, 0x1, 0x1 }, // anchor
    {  81,  168,  119, 0x4, 0x4 }, // interpolated
    {  82,  171,  123, 0x4, 0x4 }, // interpolated
    {  83,  174,  127, 0x4, 0x4 }, // interpolated
    {  84,  177,  131, 0x4, 0x4 }, // interpolated
    {  85,  180,  135, 0x4, 0x4 }, // interpolated
    {  86,  183,  139, 0x4, 0x4 }, // interpolated
    {  87,  186,  143, 0x4, 0x4 }, // interpolated
    {  88,  189,  147, 0x4, 0x4 }, // interpolated
    {  89,  192,  151, 0x4, 0x4 }, // interpolated
    {  90,  195,  156, 0x1, 0x1 }, // anchor
    {  91,  199,  159, 0x4, 0x4 }, // interpolated
    {  92,  203,  162, 0x4, 0x4 }, // interpolated
    {  93,  207,  166, 0x4, 0x4 }, // interpolated
    {  94,  211,  169, 0x4, 0x4 }, // interpolated
    {  95,  216,  173, 0x4, 0x4 }, // interpolated
    {  96,  220,  176, 0x4, 0x4 }, // interpolated
    {  97,  224,  179, 0x4, 0x4 }, // interpolated
    {  98,  228,  183, 0x4, 0x4 }, // interpolated
    {  99,  232,  186, 0x4, 0x4 }, // interpolated
    { 100,  237,  190, 0x1, 0x1 }, // anchor
};

// level, f(HMP) divisor, provenance  (published only at Lv70 264 / Lv80 304)
constexpr FfxivHealingPotencyRow kHealingPotencyTable[FFXIV_COMBAT_LEVEL_COUNT] =
{
    {   1, 264, 0x20 }, // held
    {   2, 264, 0x20 }, // held
    {   3, 264, 0x20 }, // held
    {   4, 264, 0x20 }, // held
    {   5, 264, 0x20 }, // held
    {   6, 264, 0x20 }, // held
    {   7, 264, 0x20 }, // held
    {   8, 264, 0x20 }, // held
    {   9, 264, 0x20 }, // held
    {  10, 264, 0x20 }, // held
    {  11, 264, 0x20 }, // held
    {  12, 264, 0x20 }, // held
    {  13, 264, 0x20 }, // held
    {  14, 264, 0x20 }, // held
    {  15, 264, 0x20 }, // held
    {  16, 264, 0x20 }, // held
    {  17, 264, 0x20 }, // held
    {  18, 264, 0x20 }, // held
    {  19, 264, 0x20 }, // held
    {  20, 264, 0x20 }, // held
    {  21, 264, 0x20 }, // held
    {  22, 264, 0x20 }, // held
    {  23, 264, 0x20 }, // held
    {  24, 264, 0x20 }, // held
    {  25, 264, 0x20 }, // held
    {  26, 264, 0x20 }, // held
    {  27, 264, 0x20 }, // held
    {  28, 264, 0x20 }, // held
    {  29, 264, 0x20 }, // held
    {  30, 264, 0x20 }, // held
    {  31, 264, 0x20 }, // held
    {  32, 264, 0x20 }, // held
    {  33, 264, 0x20 }, // held
    {  34, 264, 0x20 }, // held
    {  35, 264, 0x20 }, // held
    {  36, 264, 0x20 }, // held
    {  37, 264, 0x20 }, // held
    {  38, 264, 0x20 }, // held
    {  39, 264, 0x20 }, // held
    {  40, 264, 0x20 }, // held
    {  41, 264, 0x20 }, // held
    {  42, 264, 0x20 }, // held
    {  43, 264, 0x20 }, // held
    {  44, 264, 0x20 }, // held
    {  45, 264, 0x20 }, // held
    {  46, 264, 0x20 }, // held
    {  47, 264, 0x20 }, // held
    {  48, 264, 0x20 }, // held
    {  49, 264, 0x20 }, // held
    {  50, 264, 0x20 }, // held
    {  51, 264, 0x20 }, // held
    {  52, 264, 0x20 }, // held
    {  53, 264, 0x20 }, // held
    {  54, 264, 0x20 }, // held
    {  55, 264, 0x20 }, // held
    {  56, 264, 0x20 }, // held
    {  57, 264, 0x20 }, // held
    {  58, 264, 0x20 }, // held
    {  59, 264, 0x20 }, // held
    {  60, 264, 0x20 }, // held
    {  61, 264, 0x20 }, // held
    {  62, 264, 0x20 }, // held
    {  63, 264, 0x20 }, // held
    {  64, 264, 0x20 }, // held
    {  65, 264, 0x20 }, // held
    {  66, 264, 0x20 }, // held
    {  67, 264, 0x20 }, // held
    {  68, 264, 0x20 }, // held
    {  69, 264, 0x20 }, // held
    {  70, 264, 0x10 }, // anchor
    {  71, 264, 0x20 }, // held
    {  72, 264, 0x20 }, // held
    {  73, 264, 0x20 }, // held
    {  74, 264, 0x20 }, // held
    {  75, 264, 0x20 }, // held
    {  76, 264, 0x20 }, // held
    {  77, 264, 0x20 }, // held
    {  78, 264, 0x20 }, // held
    {  79, 264, 0x20 }, // held
    {  80, 304, 0x10 }, // anchor
    {  81, 304, 0x20 }, // held
    {  82, 304, 0x20 }, // held
    {  83, 304, 0x20 }, // held
    {  84, 304, 0x20 }, // held
    {  85, 304, 0x20 }, // held
    {  86, 304, 0x20 }, // held
    {  87, 304, 0x20 }, // held
    {  88, 304, 0x20 }, // held
    {  89, 304, 0x20 }, // held
    {  90, 304, 0x20 }, // held
    {  91, 304, 0x20 }, // held
    {  92, 304, 0x20 }, // held
    {  93, 304, 0x20 }, // held
    {  94, 304, 0x20 }, // held
    {  95, 304, 0x20 }, // held
    {  96, 304, 0x20 }, // held
    {  97, 304, 0x20 }, // held
    {  98, 304, 0x20 }, // held
    {  99, 304, 0x20 }, // held
    { 100, 304, 0x20 }, // held
};

// jobId, abbreviation, attribute (BaseParam), damage trait %, auto potency,
// DoT damage type  (F23.13 / F24.2 / F23.7)
constexpr FfxivJobCombatRow kJobCombatTable[FFXIV_COMBAT_JOB_COUNT] =
{
    {  0, "---", 1, 100, 90, 0 },
    {  1, "GLA", 1, 100, 90, 0 },
    {  2, "PGL", 1, 100, 90, 0 },
    {  3, "MRD", 1, 100, 90, 0 },
    {  4, "LNC", 1, 100, 90, 0 },
    {  5, "ARC", 2, 120, 80, 0 },
    {  6, "CNJ", 5, 130, 90, 1 },
    {  7, "THM", 4, 130, 90, 1 },
    {  8, "CRP", 1, 100, 90, 0 },
    {  9, "BSM", 1, 100, 90, 0 },
    { 10, "ARM", 1, 100, 90, 0 },
    { 11, "GSM", 1, 100, 90, 0 },
    { 12, "LTW", 1, 100, 90, 0 },
    { 13, "WVR", 1, 100, 90, 0 },
    { 14, "ALC", 1, 100, 90, 0 },
    { 15, "CUL", 1, 100, 90, 0 },
    { 16, "MIN", 1, 100, 90, 0 },
    { 17, "BTN", 1, 100, 90, 0 },
    { 18, "FSH", 1, 100, 90, 0 },
    { 19, "PLD", 1, 100, 90, 0 },
    { 20, "MNK", 1, 100, 90, 0 },
    { 21, "WAR", 1, 100, 90, 0 },
    { 22, "DRG", 1, 100, 90, 0 },
    { 23, "BRD", 2, 120, 80, 0 },
    { 24, "WHM", 5, 130, 90, 1 },
    { 25, "BLM", 4, 130, 90, 1 },
    { 26, "ACN", 4, 130, 90, 1 },
    { 27, "SMN", 4, 130, 90, 1 },
    { 28, "SCH", 5, 130, 90, 1 },
    { 29, "ROG", 2, 100, 90, 0 },
    { 30, "NIN", 2, 100, 90, 0 },
    { 31, "MCH", 2, 120, 80, 0 },
    { 32, "DRK", 1, 100, 90, 0 },
    { 33, "AST", 5, 130, 90, 1 },
    { 34, "SAM", 1, 100, 90, 0 },
    { 35, "RDM", 4, 130, 90, 1 },
    { 36, "BLU", 4, 150, 90, 1 },
    { 37, "GNB", 1, 100, 90, 0 },
    { 38, "DNC", 2, 120, 90, 0 },
    { 39, "RPR", 1, 100, 90, 0 },
    { 40, "SGE", 5, 130, 90, 1 },
    { 41, "VPR", 2, 100, 90, 0 },
    { 42, "PCT", 4, 130, 90, 1 },
    { 43, "BST", 1, 100, 90, 0 },
};
} // namespace FFXIV::Combat::Tables
