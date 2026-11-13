#pragma once
// FFXIV character-creation rules for the server: the wire tuple, CharaMakeType lookups and
// validation. Engine-free so it can be host tested. Port of the retired livescript
// (work/hyur-port/retired/shared/FFXIVCreationModel.ts and CreationProtocol.ts); the data is
// generated into FFXIVCreationModelData.inc by work/hyur-port/tools/gen-charamake-model.py.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace FFXIV::Creation {

// Packet opcodes (custom packet ids; see CreationProtocol.ts). Append only.
constexpr std::uint16_t SNAPSHOT_OPCODE = 0x7A34;  // server -> client: stored tuple
constexpr std::uint16_t REQUEST_OPCODE = 0x7A35;   // server -> client: please send your tuple
constexpr std::uint16_t UPDATE_OPCODE = 0x7A36;    // client -> server: save this tuple
constexpr std::uint16_t UNIT_TUPLE_OPCODE = 0x7A37;  // server -> client: another unit's stored tuple

// Version 2 appended nameday and guardian (the retired ffxivcharacterprofile row).
constexpr std::uint8_t PROTOCOL_VERSION = 2;
constexpr std::uint8_t TUPLE_VERSION = 2;
// version + race + clan + sex + 24 appearance fields + nameday moon/day + guardian.
constexpr std::size_t TUPLE_BYTES = 31;
// UPDATE payload: protocol version (1) + echoed GUID (8) + tuple (31).
constexpr std::size_t UPDATE_PAYLOAD_BYTES = 1 + 8 + TUPLE_BYTES;

constexpr std::uint8_t FIRST_PLAYABLE_RACE = 12;
constexpr std::uint8_t LAST_PLAYABLE_RACE = 20;

enum class OptionId : std::uint8_t {
    Voice = 0,
    Height,
    Face,
    Hairstyle,
    SkinColor,
    EyeColor,
    HairColor,
    FacialFeatures,
    Tattoos,
    TattooColor,
    Eyebrows,
    IrisSize,
    EyeShape,
    Nose,
    Jaw,
    Mouth,
    LipColor,
    MuscleTone,
    RaceFeature,
    BustSize,
    FacePaint,
    FacePaintColor,
    Count,
};

enum class MenuKind : std::uint8_t {
    Slider = 0,
    Icons = 1,
    Color = 2,
    DoubleColor = 3,
    List = 4,
    Checks = 5,
};

// Layout of the generated data (same field order as the client's CFXIVCreationModel.h).
struct MenuDef {
    OptionId id;
    std::uint8_t customize;
    std::uint8_t occurrence;
    std::uint16_t lobby;
    MenuKind kind;
    std::uint16_t count;
    std::uint16_t initial;
    std::uint16_t paramOffset;
    std::uint8_t paramCount;
    std::uint8_t displayOrder;
};

struct RowDef {
    std::uint8_t race;  // CharaMakeType race 1..8
    std::uint8_t clan;  // 1..2
    std::uint8_t sex;   // 0 male, 1 female
    std::uint16_t menuOffset;
    std::uint8_t menuCount;
};

struct RaceBinding {
    std::uint8_t playableRace;   // WoW race id 12..20
    const char* name;
    std::uint8_t charaMakeRace;  // RowDef::race
    std::uint8_t fixedClan;      // 0 = both clans selectable
    std::uint8_t customizeRace;
    const char* maleBodyCode;
    const char* femaleBodyCode;
};

struct BodyCodeBinding {
    const char* code;
    std::uint8_t playableRace;
    std::uint8_t sex;
};

namespace Data {
// Extents of the generated tables; the .cpp static_asserts them against the arrays.
constexpr std::size_t kRowCount = 32;
constexpr std::size_t kMenuCount = 680;
constexpr std::size_t kParamCount = 4110;
constexpr std::size_t kRaceCount = 9;
constexpr std::size_t kBodyCodeCount = 18;
extern const RowDef kRows[];
extern const MenuDef kMenus[];
extern const std::uint16_t kMenuMax[];
extern const std::uint32_t kParams[];
extern const RaceBinding kRaceBindings[];
extern const BodyCodeBinding kBodyCodes[];
}  // namespace Data

// The customise tuple, in wire order (version first).
struct CustomizeTuple {
    std::uint8_t version = TUPLE_VERSION;
    std::uint8_t race = 0;
    std::uint8_t clan = 0;
    std::uint8_t sex = 0;
    std::uint8_t face = 0;
    std::uint8_t hairstyle = 0;
    std::uint8_t hairColor = 0;
    std::uint8_t highlightColor = 0;
    std::uint8_t skin = 0;
    std::uint8_t eyeLeft = 0;
    std::uint8_t eyeRight = 0;
    std::uint8_t lips = 0;
    std::uint8_t tattooColor = 0;
    std::uint8_t facialFeatures = 0;
    std::uint8_t tattoos = 0;
    std::uint8_t facePaint = 0;
    std::uint8_t facePaintColor = 0;
    std::uint8_t muscle = 0;
    std::uint8_t bust = 0;
    std::uint8_t height = 0;
    std::uint8_t voice = 0;
    std::uint8_t eyebrows = 0;
    std::uint8_t nose = 0;
    std::uint8_t jaw = 0;
    std::uint8_t eyeShape = 0;
    std::uint8_t irisSize = 0;
    std::uint8_t raceFeature = 0;
    std::uint8_t flags = 0;
    // Not appearance: the creation screen's nameday and guardian deity. 0 = not chosen.
    std::uint8_t namedayMoon = 0;  // 1..12
    std::uint8_t namedayDay = 0;   // 1..32
    std::uint8_t guardian = 0;     // GuardianDeity row 1..12
};

// Reads exactly TUPLE_BYTES from data. False when the buffer is too short.
bool ReadTuple(std::uint8_t const* data, std::size_t size, CustomizeTuple& out);
void WriteTuple(CustomizeTuple const& tuple, std::vector<std::uint8_t>& out);

// The tuple's own shape check (version, playable race range, sex, clan, flags).
bool HasValidShape(CustomizeTuple const& tuple);

// The clan a playable race resolves to (fixed clans override the requested one).
std::uint8_t ResolveClan(std::uint8_t playableRace, std::uint8_t clan);

RaceBinding const* FindRace(std::uint8_t playableRace);
RowDef const* FindRow(std::uint8_t charaMakeRace, std::uint8_t clan, std::uint8_t sex);

// Largest value a menu accepts, or 0 when the row has no such menu.
std::uint16_t MenuMax(RowDef const& row, OptionId id);

// Empty string when the tuple is valid, otherwise the first offending field name.
char const* ValidateTuple(CustomizeTuple const& tuple);

}  // namespace FFXIV::Creation
