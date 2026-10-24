CREATE TABLE IF NOT EXISTS `ffxiv_class_switch_config`
(
    `id` TINYINT UNSIGNED NOT NULL,
    `switch_spell_id` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `ffxiv_class_switch_classes`
(
    `class_id` TINYINT UNSIGNED NOT NULL,
    `initial_level` TINYINT UNSIGNED NOT NULL,
    `race_mask` INT UNSIGNED NOT NULL,
    `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `display_name` VARCHAR(64) NOT NULL,
    PRIMARY KEY (`class_id`),
    CONSTRAINT `ffxiv_class_switch_id_range`
        CHECK (`class_id` BETWEEN 1 AND 32),
    CONSTRAINT `ffxiv_class_switch_level_range`
        CHECK (`initial_level` BETWEEN 1 AND 80),
    CONSTRAINT `ffxiv_class_switch_race_mask`
        CHECK (`race_mask` <> 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
