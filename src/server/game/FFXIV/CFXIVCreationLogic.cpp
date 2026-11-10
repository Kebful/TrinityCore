#include "CFXIVCreationLogic.h"

namespace FFXIV::Creation {

// Generated tables (kRows, kMenus, kMenuMax, kParams, kRaceBindings, kBodyCodes), in namespace Data.
#include "FFXIVCreationModelData.inc"

static_assert(sizeof(Data::kRows) / sizeof(Data::kRows[0]) == Data::kRowCount, "row table extent");
static_assert(sizeof(Data::kMenus) / sizeof(Data::kMenus[0]) == Data::kMenuCount, "menu table extent");
static_assert(sizeof(Data::kMenuMax) / sizeof(Data::kMenuMax[0]) == Data::kMenuCount, "menu max extent");
static_assert(sizeof(Data::kParams) / sizeof(Data::kParams[0]) == Data::kParamCount, "param table extent");
static_assert(sizeof(Data::kRaceBindings) / sizeof(Data::kRaceBindings[0]) == Data::kRaceCount, "race extent");
static_assert(sizeof(Data::kBodyCodes) / sizeof(Data::kBodyCodes[0]) == Data::kBodyCodeCount, "body code extent");

namespace {

// Wire order of the 31 tuple bytes (version first).
std::uint8_t* FieldRefs(CustomizeTuple& t, std::size_t index)
{
    std::uint8_t* const fields[TUPLE_BYTES] = {
        &t.version, &t.race, &t.clan, &t.sex, &t.face, &t.hairstyle, &t.hairColor, &t.highlightColor,
        &t.skin, &t.eyeLeft, &t.eyeRight, &t.lips, &t.tattooColor, &t.facialFeatures, &t.tattoos,
        &t.facePaint, &t.facePaintColor, &t.muscle, &t.bust, &t.height, &t.voice, &t.eyebrows, &t.nose,
        &t.jaw, &t.eyeShape, &t.irisSize, &t.raceFeature, &t.flags, &t.namedayMoon, &t.namedayDay,
        &t.guardian,
    };
    return fields[index];
}

std::uint8_t const* FieldRefs(CustomizeTuple const& t, std::size_t index)
{
    return FieldRefs(const_cast<CustomizeTuple&>(t), index);
}

}  // namespace

bool ReadTuple(std::uint8_t const* data, std::size_t size, CustomizeTuple& out)
{
    if (!data || size < TUPLE_BYTES)
        return false;
    for (std::size_t i = 0; i < TUPLE_BYTES; ++i)
        *FieldRefs(out, i) = data[i];
    return true;
}

void WriteTuple(CustomizeTuple const& tuple, std::vector<std::uint8_t>& out)
{
    for (std::size_t i = 0; i < TUPLE_BYTES; ++i)
        out.push_back(*FieldRefs(tuple, i));
}

bool HasValidShape(CustomizeTuple const& tuple)
{
    return tuple.version == TUPLE_VERSION
        && tuple.race >= FIRST_PLAYABLE_RACE
        && tuple.race <= LAST_PLAYABLE_RACE
        && tuple.sex <= 1
        && tuple.clan >= 1
        && tuple.clan <= 2
        && tuple.flags <= 7;
}

std::uint8_t ResolveClan(std::uint8_t playableRace, std::uint8_t clan)
{
    if (RaceBinding const* race = FindRace(playableRace))
    {
        if (race->fixedClan != 0)
            return race->fixedClan;
    }
    return clan == 2 ? 2 : 1;
}

RaceBinding const* FindRace(std::uint8_t playableRace)
{
    for (std::size_t i = 0; i < Data::kRaceCount; ++i)
    {
        if (Data::kRaceBindings[i].playableRace == playableRace)
            return &Data::kRaceBindings[i];
    }
    return nullptr;
}

RowDef const* FindRow(std::uint8_t charaMakeRace, std::uint8_t clan, std::uint8_t sex)
{
    for (std::size_t i = 0; i < Data::kRowCount; ++i)
    {
        RowDef const& row = Data::kRows[i];
        if (row.race == charaMakeRace && row.clan == clan && row.sex == sex)
            return &row;
    }
    return nullptr;
}

std::uint16_t MenuMax(RowDef const& row, OptionId id)
{
    for (std::uint8_t i = 0; i < row.menuCount; ++i)
    {
        std::size_t const index = static_cast<std::size_t>(row.menuOffset) + i;
        if (Data::kMenus[index].id == id)
            return Data::kMenuMax[index];
    }
    return 0;
}

char const* ValidateTuple(CustomizeTuple const& tuple)
{
    if (!HasValidShape(tuple))
        return "version";

    RaceBinding const* race = FindRace(tuple.race);
    if (!race)
        return "race";
    std::uint8_t const clan = ResolveClan(tuple.race, tuple.clan);
    RowDef const* row = FindRow(race->charaMakeRace, clan, tuple.sex);
    if (!row)
        return "race";

    struct Check { std::uint8_t value; OptionId option; char const* field; };
    Check const checks[] = {
        { tuple.face, OptionId::Face, "face" },
        { tuple.hairstyle, OptionId::Hairstyle, "hairstyle" },
        { tuple.hairColor, OptionId::HairColor, "hairColor" },
        { tuple.highlightColor, OptionId::HairColor, "highlightColor" },
        { tuple.skin, OptionId::SkinColor, "skin" },
        { tuple.eyeLeft, OptionId::EyeColor, "eyeLeft" },
        { tuple.eyeRight, OptionId::EyeColor, "eyeRight" },
        { tuple.lips, OptionId::LipColor, "lips" },
        { tuple.tattooColor, OptionId::TattooColor, "tattooColor" },
        { tuple.facialFeatures, OptionId::FacialFeatures, "facialFeatures" },
        { tuple.tattoos, OptionId::Tattoos, "tattoos" },
        { tuple.facePaint, OptionId::FacePaint, "facePaint" },
        { tuple.facePaintColor, OptionId::FacePaintColor, "facePaintColor" },
        { tuple.muscle, OptionId::MuscleTone, "muscle" },
        { tuple.bust, OptionId::BustSize, "bust" },
        { tuple.height, OptionId::Height, "height" },
        { tuple.voice, OptionId::Voice, "voice" },
        { tuple.eyebrows, OptionId::Eyebrows, "eyebrows" },
        { tuple.nose, OptionId::Nose, "nose" },
        { tuple.jaw, OptionId::Jaw, "jaw" },
        { tuple.eyeShape, OptionId::EyeShape, "eyeShape" },
        { tuple.irisSize, OptionId::IrisSize, "irisSize" },
        { tuple.raceFeature, OptionId::RaceFeature, "raceFeature" },
    };
    for (Check const& check : checks)
    {
        if (check.value > MenuMax(*row, check.option))
            return check.field;
    }
    if (tuple.clan != clan)
        return "clan";
    if (tuple.flags > 7)
        return "flags";
    // Nameday (12 moons of 32 suns, Lobby row 37) and guardian (GuardianDeity rows 1..12)
    // are optional; when set, both nameday parts must be.
    if (tuple.namedayMoon > 12 || tuple.namedayDay > 32 || (tuple.namedayMoon == 0) != (tuple.namedayDay == 0))
        return "nameday";
    if (tuple.guardian > 12)
        return "guardian";
    return "";
}

}  // namespace FFXIV::Creation
