-- Shared class/job progression, keyed the way FFXIV keys it: one row per ExpArrayIndex/WorkIndex.
--
-- Profiles stay per class (spellbook, skills, bars, equipment). Level and EXP live here so a class and
-- its job read and write the same progression. Existing characters are seeded on first load from the
-- highest-level profile in their group (PlayerClassSwitch.cpp, LoadOrSeedProgress).
CREATE TABLE IF NOT EXISTS `ffxiv_class_switch_progress`
(
    `guid` INT UNSIGNED NOT NULL,
    `exp_group` TINYINT UNSIGNED NOT NULL,
    `level` TINYINT UNSIGNED NOT NULL,
    `xp` INT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
        ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`guid`, `exp_group`),
    CONSTRAINT `ffxiv_class_switch_progress_character`
        FOREIGN KEY (`guid`) REFERENCES `characters` (`guid`)
        ON DELETE CASCADE,
    CONSTRAINT `ffxiv_class_switch_progress_group_range`
        CHECK (`exp_group` BETWEEN 1 AND 32),
    CONSTRAINT `ffxiv_class_switch_progress_level_range`
        CHECK (`level` BETWEEN 1 AND 80)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
