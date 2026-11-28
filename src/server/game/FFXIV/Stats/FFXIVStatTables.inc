// GENERATED FILE -- do not edit.
// Generator: work/stat-research/gen_ffxiv_stat_tables.py
// Sheets:    work/exd-all/exd (ParamGrow c5/c6, ClassJob c0/c3/c9-16/c33, Tribe c2-9)
// Model:     B-FORMULA-REFERENCE F1/F3 level anchors 1/50/60/70/80 (levels without
//            an anchor are linearly interpolated and carry provenance bit 0x4).
// Schema:    1
// Manifest:  fnv1a32 0x67BC815C  sha256 699cbbcfd8a56a61e954bbd755df2f48f092d421dd2123bb8634853b3584fa45

namespace FFXIV::Stats::Tables
{
constexpr uint32 kManifest = 0x67BC815C;
constexpr uint32 kSchemaVersion = 1;
constexpr char const* kManifestSha256 = "699cbbcfd8a56a61e954bbd755df2f48f092d421dd2123bb8634853b3584fa45";
constexpr char const* kSourceSheets = "work/exd-all/exd";

// level, mainStat(model), baseSubStat(sheet c5), levelDiv(sheet c6), baseHp(model),
// hpScalarTankTenths(model), hpScalarOtherTenths(model), levelProvenance
constexpr FfxivLevelRow kLevelTable[FFXIV_LEVEL_COUNT] =
{
    {  1,  20,  56,   56,   86,   0,   0, 0 }, // anchor (ParamGrow c8 18, not used)
    {  2,  21,  57,   57,  101,   3,   2, 4 }, // interpolated (ParamGrow c8 20, not used)
    {  3,  22,  60,   60,  109,   6,   4, 4 }, // interpolated (ParamGrow c8 21, not used)
    {  4,  24,  62,   62,  116,   9,   6, 4 }, // interpolated (ParamGrow c8 27, not used)
    {  5,  26,  65,   65,  123,  12,   9, 4 }, // interpolated (ParamGrow c8 34, not used)
    {  6,  27,  68,   68,  131,  15,  11, 4 }, // interpolated (ParamGrow c8 35, not used)
    {  7,  29,  70,   70,  138,  18,  13, 4 }, // interpolated (ParamGrow c8 40, not used)
    {  8,  31,  73,   73,  145,  22,  16, 4 }, // interpolated (ParamGrow c8 43, not used)
    {  9,  33,  76,   76,  153,  25,  18, 4 }, // interpolated (ParamGrow c8 46, not used)
    { 10,  35,  78,   78,  160,  28,  20, 4 }, // interpolated (ParamGrow c8 48, not used)
    { 11,  36,  82,   82,  174,  31,  22, 4 }, // interpolated (ParamGrow c8 55, not used)
    { 12,  38,  85,   85,  188,  34,  25, 4 }, // interpolated (ParamGrow c8 58, not used)
    { 13,  41,  89,   89,  202,  37,  27, 4 }, // interpolated (ParamGrow c8 63, not used)
    { 14,  44,  93,   93,  216,  41,  29, 4 }, // interpolated (ParamGrow c8 64, not used)
    { 15,  46,  96,   96,  230,  44,  32, 4 }, // interpolated (ParamGrow c8 69, not used)
    { 16,  49, 100,  100,  244,  47,  34, 4 }, // interpolated (ParamGrow c8 75, not used)
    { 17,  52, 104,  104,  258,  50,  36, 4 }, // interpolated (ParamGrow c8 81, not used)
    { 18,  54, 109,  109,  272,  53,  38, 4 }, // interpolated (ParamGrow c8 86, not used)
    { 19,  57, 113,  113,  286,  56,  41, 4 }, // interpolated (ParamGrow c8 90, not used)
    { 20,  60, 116,  116,  300,  60,  43, 4 }, // interpolated (ParamGrow c8 95, not used)
    { 21,  63, 122,  122,  333,  63,  45, 4 }, // interpolated (ParamGrow c8 102, not used)
    { 22,  67, 127,  127,  366,  66,  48, 4 }, // interpolated (ParamGrow c8 108, not used)
    { 23,  71, 133,  133,  399,  69,  50, 4 }, // interpolated (ParamGrow c8 115, not used)
    { 24,  74, 138,  138,  432,  72,  52, 4 }, // interpolated (ParamGrow c8 121, not used)
    { 25,  78, 144,  144,  465,  75,  54, 4 }, // interpolated (ParamGrow c8 127, not used)
    { 26,  81, 150,  150,  498,  79,  57, 4 }, // interpolated (ParamGrow c8 147, not used)
    { 27,  85, 155,  155,  531,  82,  59, 4 }, // interpolated (ParamGrow c8 157, not used)
    { 28,  89, 162,  162,  564,  85,  61, 4 }, // interpolated (ParamGrow c8 174, not used)
    { 29,  92, 168,  168,  597,  88,  64, 4 }, // interpolated (ParamGrow c8 184, not used)
    { 30,  97, 173,  173,  630,  91,  66, 4 }, // interpolated (ParamGrow c8 192, not used)
    { 31, 101, 181,  181,  669,  94,  68, 4 }, // interpolated (ParamGrow c8 208, not used)
    { 32, 106, 188,  188,  708,  98,  70, 4 }, // interpolated (ParamGrow c8 232, not used)
    { 33, 110, 194,  194,  747, 101,  73, 4 }, // interpolated (ParamGrow c8 249, not used)
    { 34, 115, 202,  202,  786, 104,  75, 4 }, // interpolated (ParamGrow c8 266, not used)
    { 35, 119, 209,  209,  825, 107,  77, 4 }, // interpolated (ParamGrow c8 286, not used)
    { 36, 124, 215,  215,  864, 110,  80, 4 }, // interpolated (ParamGrow c8 317, not used)
    { 37, 128, 223,  223,  903, 113,  82, 4 }, // interpolated (ParamGrow c8 339, not used)
    { 38, 134, 229,  229,  942, 117,  84, 4 }, // interpolated (ParamGrow c8 360, not used)
    { 39, 139, 236,  236,  981, 120,  86, 4 }, // interpolated (ParamGrow c8 384, not used)
    { 40, 144, 244,  244, 1020, 123,  89, 4 }, // interpolated (ParamGrow c8 419, not used)
    { 41, 150, 253,  253, 1088, 126,  91, 4 }, // interpolated (ParamGrow c8 441, not used)
    { 42, 155, 263,  263, 1156, 129,  93, 4 }, // interpolated (ParamGrow c8 462, not used)
    { 43, 161, 272,  272, 1224, 132,  96, 4 }, // interpolated (ParamGrow c8 484, not used)
    { 44, 166, 283,  283, 1292, 136,  98, 4 }, // interpolated (ParamGrow c8 505, not used)
    { 45, 171, 292,  292, 1360, 139, 100, 4 }, // interpolated (ParamGrow c8 527, not used)
    { 46, 177, 302,  302, 1428, 142, 102, 4 }, // interpolated (ParamGrow c8 578, not used)
    { 47, 183, 311,  311, 1496, 145, 105, 4 }, // interpolated (ParamGrow c8 619, not used)
    { 48, 189, 322,  322, 1564, 148, 107, 4 }, // interpolated (ParamGrow c8 659, not used)
    { 49, 196, 331,  331, 1632, 151, 109, 4 }, // interpolated (ParamGrow c8 700, not used)
    { 50, 202, 341,  341, 1700, 155, 112, 0 }, // anchor (ParamGrow c8 927, not used)
    { 51, 204, 342,  366, 1500, 157, 113, 4 }, // interpolated (ParamGrow c8 1050, not used)
    { 52, 205, 344,  392, 1500, 159, 115, 4 }, // interpolated (ParamGrow c8 1160, not used)
    { 53, 207, 345,  418, 1500, 161, 117, 4 }, // interpolated (ParamGrow c8 1260, not used)
    { 54, 209, 346,  444, 1500, 163, 118, 4 }, // interpolated (ParamGrow c8 1386, not used)
    { 55, 210, 347,  470, 1500, 165, 120, 4 }, // interpolated (ParamGrow c8 1507, not used)
    { 56, 212, 349,  496, 1500, 167, 122, 4 }, // interpolated (ParamGrow c8 1623, not used)
    { 57, 214, 350,  522, 1500, 169, 123, 4 }, // interpolated (ParamGrow c8 1733, not used)
    { 58, 215, 351,  548, 1500, 171, 125, 4 }, // interpolated (ParamGrow c8 1740, not used)
    { 59, 217, 352,  574, 1500, 173, 127, 4 }, // interpolated (ParamGrow c8 1756, not used)
    { 60, 218, 354,  600, 1500, 175, 129, 0 }, // anchor (ParamGrow c8 1767, not used)
    { 61, 224, 355,  630, 1700, 176, 130, 4 }, // interpolated (ParamGrow c8 1863, not used)
    { 62, 228, 356,  660, 1700, 177, 131, 4 }, // interpolated (ParamGrow c8 1956, not used)
    { 63, 236, 357,  690, 1700, 178, 132, 4 }, // interpolated (ParamGrow c8 2057, not used)
    { 64, 244, 358,  720, 1700, 180, 133, 4 }, // interpolated (ParamGrow c8 2191, not used)
    { 65, 252, 359,  750, 1700, 181, 134, 4 }, // interpolated (ParamGrow c8 2217, not used)
    { 66, 260, 360,  780, 1700, 182, 135, 4 }, // interpolated (ParamGrow c8 2224, not used)
    { 67, 268, 361,  810, 1700, 184, 136, 4 }, // interpolated (ParamGrow c8 2230, not used)
    { 68, 276, 362,  840, 1700, 185, 137, 4 }, // interpolated (ParamGrow c8 2247, not used)
    { 69, 284, 363,  870, 1700, 186, 138, 4 }, // interpolated (ParamGrow c8 2251, not used)
    { 70, 292, 364,  900, 1700, 188, 140, 0 }, // anchor (ParamGrow c8 2275, not used)
    { 71, 296, 365,  940, 2000, 195, 144, 4 }, // interpolated (ParamGrow c8 2275, not used)
    { 72, 300, 366,  980, 2000, 203, 149, 4 }, // interpolated (ParamGrow c8 2336, not used)
    { 73, 305, 367, 1020, 2000, 211, 154, 4 }, // interpolated (ParamGrow c8 2419, not used)
    { 74, 310, 368, 1060, 2000, 219, 159, 4 }, // interpolated (ParamGrow c8 2596, not used)
    { 75, 315, 370, 1100, 2000, 227, 164, 4 }, // interpolated (ParamGrow c8 2770, not used)
    { 76, 320, 372, 1140, 2000, 234, 168, 4 }, // interpolated (ParamGrow c8 2974, not used)
    { 77, 325, 374, 1180, 2000, 242, 173, 4 }, // interpolated (ParamGrow c8 3200, not used)
    { 78, 330, 376, 1220, 2000, 250, 178, 4 }, // interpolated (ParamGrow c8 3452, not used)
    { 79, 335, 378, 1260, 2000, 258, 183, 4 }, // interpolated (ParamGrow c8 3733, not used)
    { 80, 340, 380, 1300, 2000, 266, 188, 0 }, // anchor (ParamGrow c8 3121, not used)
    { 81, 345, 382, 1360, 2100, 274, 193, 4 }, // interpolated (ParamGrow c8 3175, not used)
    { 82, 350, 384, 1420, 2200, 282, 199, 4 }, // interpolated (ParamGrow c8 3229, not used)
    { 83, 355, 386, 1480, 2300, 290, 204, 4 }, // interpolated (ParamGrow c8 3283, not used)
    { 84, 360, 388, 1540, 2400, 298, 210, 4 }, // interpolated (ParamGrow c8 3337, not used)
    { 85, 365, 390, 1600, 2500, 306, 215, 4 }, // interpolated (ParamGrow c8 3391, not used)
    { 86, 370, 392, 1660, 2600, 314, 221, 4 }, // interpolated (ParamGrow c8 3445, not used)
    { 87, 375, 394, 1720, 2700, 322, 226, 4 }, // interpolated (ParamGrow c8 3499, not used)
    { 88, 380, 396, 1780, 2800, 330, 232, 4 }, // interpolated (ParamGrow c8 3553, not used)
    { 89, 385, 398, 1840, 2900, 338, 237, 4 }, // interpolated (ParamGrow c8 3607, not used)
    { 90, 390, 400, 1900, 3000, 346, 243, 0 }, // anchor (ParamGrow c8 3661, not used)
    { 91, 395, 402, 1988, 3100, 354, 248, 4 }, // interpolated (ParamGrow c8 3716, not used)
    { 92, 400, 404, 2076, 3200, 362, 254, 4 }, // interpolated (ParamGrow c8 3770, not used)
    { 93, 405, 406, 2164, 3300, 371, 260, 4 }, // interpolated (ParamGrow c8 3824, not used)
    { 94, 410, 408, 2252, 3400, 379, 266, 4 }, // interpolated (ParamGrow c8 3879, not used)
    { 95, 415, 410, 2340, 3500, 388, 272, 4 }, // interpolated (ParamGrow c8 3933, not used)
    { 96, 420, 412, 2428, 3600, 396, 277, 4 }, // interpolated (ParamGrow c8 3987, not used)
    { 97, 425, 414, 2516, 3700, 404, 283, 4 }, // interpolated (ParamGrow c8 4042, not used)
    { 98, 430, 416, 2604, 3800, 413, 289, 4 }, // interpolated (ParamGrow c8 4096, not used)
    { 99, 435, 418, 2692, 3900, 421, 295, 4 }, // interpolated (ParamGrow c8 4150, not used)
    { 100, 440, 420, 2780, 4000, 430, 301, 0 }, // anchor (ParamGrow c8 4205, not used)
};

// jobId, abbreviation, jobType(sheet c3; 30 DoW, 31 DoM, 32 DoL, 33 DoH), role(sheet c33; 1 tank),
// hpPct, mpPct, strPct, vitPct, dexPct, intPct, mndPct, piePct  (all ClassJob c9-16)
constexpr FfxivJobRow kJobTable[FFXIV_JOB_COUNT] =
{
    {  0, "---", 30, 0, 100, 100, 100, 100, 100, 100, 100, 100 },
    {  1, "GLA", 30, 1, 130, 100,  95, 100,  90,  50,  95, 100 },
    {  2, "PGL", 30, 2, 105, 100, 100,  95, 100,  45,  85, 100 },
    {  3, "MRD", 30, 1, 135, 100, 100, 100,  90,  30,  50, 100 },
    {  4, "LNC", 30, 2, 110, 100, 105, 100,  95,  40,  60, 100 },
    {  5, "ARC", 30, 3, 100, 100,  85,  95, 105,  80,  75, 100 },
    {  6, "CNJ", 31, 4, 100, 100,  50,  95, 100, 100, 105, 100 },
    {  7, "THM", 31, 3, 100, 100,  40,  95,  95, 105,  70, 100 },
    {  8, "CRP", 33, 0, 100, 100,  90, 105, 100,  90,  90, 100 },
    {  9, "BSM", 33, 0, 100, 100, 105,  90,  90,  90, 100, 100 },
    { 10, "ARM", 33, 0, 100, 100, 100, 105,  90,  90,  90, 100 },
    { 11, "GSM", 33, 0, 100, 100,  90,  90, 105, 100,  90, 100 },
    { 12, "LTW", 33, 0, 100, 100,  90, 105,  90, 100,  90, 100 },
    { 13, "WVR", 33, 0, 100, 100,  90,  90, 105,  90, 100, 100 },
    { 14, "ALC", 33, 0, 100, 100,  90,  90,  90, 105,  90, 100 },
    { 15, "CUL", 33, 0, 100, 100,  90,  90,  90,  90, 105, 100 },
    { 16, "MIN", 32, 0, 100, 100, 100,  90,  90,  90, 100, 100 },
    { 17, "BTN", 32, 0, 100, 100,  90,  90, 100, 100,  90, 100 },
    { 18, "FSH", 32, 0, 100, 100, 100, 100, 100, 100, 100, 100 },
    { 19, "PLD", 30, 1, 140, 100, 100, 110,  95,  60, 100, 100 },
    { 20, "MNK", 30, 2, 110, 100, 110, 100, 105,  50,  90, 100 },
    { 21, "WAR", 30, 1, 145, 100, 105, 110,  95,  40,  55, 100 },
    { 22, "DRG", 30, 2, 115, 100, 115, 105, 100,  45,  65, 100 },
    { 23, "BRD", 30, 3, 105, 100,  90, 100, 115,  85,  80, 100 },
    { 24, "WHM", 31, 4, 105, 100,  55, 100, 105, 105, 115, 100 },
    { 25, "BLM", 31, 3, 105, 100,  45, 100, 100, 115,  75, 100 },
    { 26, "ACN", 31, 3, 100, 100,  85,  95,  95, 105,  75, 100 },
    { 27, "SMN", 31, 3, 105, 100,  90, 100, 100, 115,  80, 100 },
    { 28, "SCH", 31, 4, 105, 100,  90, 100, 100, 105, 115, 100 },
    { 29, "ROG", 30, 2, 103, 100,  80,  95, 100,  60,  70, 100 },
    { 30, "NIN", 30, 2, 108, 100,  85, 100, 110,  65,  75, 100 },
    { 31, "MCH", 30, 3, 105, 100,  85, 100, 115,  80,  85, 100 },
    { 32, "DRK", 30, 1, 140, 100, 105, 110,  95,  60,  40, 100 },
    { 33, "AST", 31, 4, 105, 100,  50, 100, 100, 105, 115, 100 },
    { 34, "SAM", 30, 2, 109, 100, 112, 100, 108,  60,  50, 100 },
    { 35, "RDM", 31, 3, 105, 100,  55, 100, 105, 115, 110, 100 },
    { 36, "BLU", 31, 3, 105, 100,  70, 100, 110, 115, 105, 100 },
    { 37, "GNB", 30, 1, 140, 100, 100, 110,  95,  60, 100, 100 },
    { 38, "DNC", 30, 3, 105, 100,  90, 100, 115,  85,  80, 100 },
    { 39, "RPR", 30, 2, 115, 100, 115, 105, 100,  80,  40, 100 },
    { 40, "SGE", 31, 4, 105, 100,  60, 100, 100, 115, 115, 100 },
    { 41, "VPR", 30, 2, 111, 100, 100, 100, 110,  45,  55, 100 },
    { 42, "PCT", 31, 3, 105, 100,  50, 100, 110, 115,  80, 100 },
    { 43, "BST", 30, 2, 110, 100, 110, 100, 105,  50,  90, 100 },
};

// tribeId, hp, mp, str, vit, dex, int, mnd, pie  (Tribe c2-9, all Hp/Mp/PIE are 0 in this sheet)
constexpr FfxivTribeRow kTribeTable[FFXIV_TRIBE_COUNT] =
{
    {  0,  0,  0,   0,   0,   0,   0,   0,   0 },
    {  1,  0,  0,   2,   0,  -1,   3,  -1,   0 },
    {  2,  0,  0,   3,   2,   0,  -2,   0,   0 },
    {  3,  0,  0,   0,  -1,   3,   2,  -1,   0 },
    {  4,  0,  0,   0,  -1,   0,   3,   1,   0 },
    {  5,  0,  0,  -1,  -1,   3,   2,   0,   0 },
    {  6,  0,  0,  -1,  -2,   1,   2,   3,   0 },
    {  7,  0,  0,   2,   0,   3,  -1,  -1,   0 },
    {  8,  0,  0,  -1,  -2,   2,   1,   3,   0 },
    {  9,  0,  0,   2,   3,  -1,  -2,   1,   0 },
    { 10,  0,  0,   0,   3,  -2,   0,   2,   0 },
    { 11,  0,  0,  -1,  -1,   2,   0,   3,   0 },
    { 12,  0,  0,   3,   2,   0,   0,  -2,   0 },
    { 13,  0,  0,   3,   3,  -3,  -3,   3,   0 },
    { 14,  0,  0,   3,   3,  -3,  -3,   3,   0 },
    { 15,  0,  0,   0,  -2,   3,   1,   1,   0 },
    { 16,  0,  0,  -1,  -1,   0,   3,   2,   0 },
};

// Module class id (characters.class / ChrClasses 12..42) -> ClassJob row (the FFXIV
// job id), resolved by matching the module class display name to ClassJob column 0.
// Every module class is listed; classJobId 0 means no name match and must never
// ship (kClassJobMapUnmatched is asserted to 0 at build time). Not part of the
// manifest payload: the whole-file --check and the C++ static_asserts guard it.
// classId, classJobId, module class display name
constexpr FfxivClassJobMapRow kClassJobMap[FFXIV_MODULE_CLASS_COUNT] =
{
    { 12,  1, "Gladiator" }, // ClassJob  1 gladiator GLA
    { 13,  3, "Marauder" }, // ClassJob  3 marauder MRD
    { 14,  4, "Lancer" }, // ClassJob  4 lancer LNC
    { 15,  2, "Pugilist" }, // ClassJob  2 pugilist PGL
    { 16, 29, "Rogue" }, // ClassJob 29 rogue ROG
    { 17,  5, "Archer" }, // ClassJob  5 archer ARC
    { 18,  6, "Conjurer" }, // ClassJob  6 conjurer CNJ
    { 19,  7, "Thaumaturge" }, // ClassJob  7 thaumaturge THM
    { 20, 26, "Arcanist" }, // ClassJob 26 arcanist ACN
    { 21, 19, "Paladin" }, // ClassJob 19 paladin PLD
    { 22, 21, "Warrior" }, // ClassJob 21 warrior WAR
    { 23, 22, "Dragoon" }, // ClassJob 22 dragoon DRG
    { 24, 20, "Monk" }, // ClassJob 20 monk MNK
    { 25, 30, "Ninja" }, // ClassJob 30 ninja NIN
    { 26, 23, "Bard" }, // ClassJob 23 bard BRD
    { 27, 24, "White Mage" }, // ClassJob 24 white mage WHM
    { 28, 25, "Black Mage" }, // ClassJob 25 black mage BLM
    { 29, 27, "Summoner" }, // ClassJob 27 summoner SMN
    { 30, 28, "Scholar" }, // ClassJob 28 scholar SCH
    { 31, 32, "Dark Knight" }, // ClassJob 32 dark knight DRK
    { 32, 33, "Astrologian" }, // ClassJob 33 astrologian AST
    { 33, 31, "Machinist" }, // ClassJob 31 machinist MCH
    { 34, 34, "Samurai" }, // ClassJob 34 samurai SAM
    { 35, 35, "Red Mage" }, // ClassJob 35 red mage RDM
    { 37, 37, "Gunbreaker" }, // ClassJob 37 gunbreaker GNB
    { 38, 38, "Dancer" }, // ClassJob 38 dancer DNC
    { 39, 39, "Reaper" }, // ClassJob 39 reaper RPR
    { 40, 40, "Sage" }, // ClassJob 40 sage SGE
    { 41, 41, "Viper" }, // ClassJob 41 viper VPR
    { 42, 42, "Pictomancer" }, // ClassJob 42 pictomancer PCT
};
constexpr uint8 kClassJobMapUnmatched = 0;
} // namespace FFXIV::Stats::Tables
