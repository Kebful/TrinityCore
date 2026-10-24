CREATE TABLE IF NOT EXISTS `ffxiv_class_switch_profiles`
(
    `guid` INT UNSIGNED NOT NULL,
    `class_id` TINYINT UNSIGNED NOT NULL,
    `revision` BIGINT UNSIGNED NOT NULL,
    `level` TINYINT UNSIGNED NOT NULL,
    `xp` INT UNSIGNED NOT NULL,
    `profile` LONGBLOB NOT NULL,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
        ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`guid`, `class_id`),
    CONSTRAINT `ffxiv_class_switch_profile_character`
        FOREIGN KEY (`guid`) REFERENCES `characters` (`guid`)
        ON DELETE CASCADE,
    CONSTRAINT `ffxiv_class_switch_profile_id_range`
        CHECK (`class_id` BETWEEN 1 AND 32),
    CONSTRAINT `ffxiv_class_switch_profile_level_range`
        CHECK (`level` BETWEEN 1 AND 80)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
