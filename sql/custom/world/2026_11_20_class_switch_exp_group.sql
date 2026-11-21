-- FFXIV class/job shared progression slot.
--
-- Retail stores level/EXP per ExpArrayIndex (ClientStructs) / WorkIndex (Sapphire), not per ClassJob:
-- a class and its job share one slot. exp_group is that slot here.
--
-- 0 means "this class is its own group"; the core replaces it with class_id on load. Job rows point at
-- their base class (for example Paladin -> Gladiator).
SET @has_exp_group = (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'ffxiv_class_switch_classes'
      AND COLUMN_NAME = 'exp_group'
);
SET @add_exp_group = IF(@has_exp_group = 0,
    'ALTER TABLE `ffxiv_class_switch_classes` ADD COLUMN `exp_group` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `class_id`',
    'SELECT 1');
PREPARE add_exp_group FROM @add_exp_group;
EXECUTE add_exp_group;
DEALLOCATE PREPARE add_exp_group;

UPDATE `ffxiv_class_switch_classes`
SET `exp_group` = `class_id`
WHERE `exp_group` = 0;
