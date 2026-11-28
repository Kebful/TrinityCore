-- AllowableClass uses signed two's complement so the existing -1 wildcard
-- remains an all-class mask. The other world masks use unsigned storage.
SET @legacy_item_class_mask = (
    SELECT DATA_TYPE = 'int' FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'item_template' AND COLUMN_NAME = 'AllowableClass'
);
ALTER TABLE `item_template` MODIFY `AllowableClass` BIGINT NOT NULL DEFAULT -1;
ALTER TABLE `spell_autolearn` MODIFY `classmask` BIGINT UNSIGNED NOT NULL DEFAULT 0;
ALTER TABLE `playercreateinfo_skills` MODIFY `classMask` BIGINT UNSIGNED NOT NULL;
ALTER TABLE `playercreateinfo_spell_custom` MODIFY `classmask` BIGINT UNSIGNED NOT NULL DEFAULT 0;
ALTER TABLE `playercreateinfo_cast_spell` MODIFY `classMask` BIGINT UNSIGNED NOT NULL DEFAULT 0;
ALTER TABLE `trainer` MODIFY `classMask` BIGINT UNSIGNED NOT NULL DEFAULT 0;
ALTER TABLE `trainer_spell` MODIFY `classMask` BIGINT UNSIGNED NOT NULL DEFAULT 0;
ALTER TABLE `npc_vendor` MODIFY `classMask` BIGINT UNSIGNED NOT NULL DEFAULT 0;

-- Preserve the low 32-bit masks stored as negative INT values. Keep -1 as the
-- explicit wildcard. Existing BIGINT masks are untouched on repeat runs.
UPDATE `item_template` SET `AllowableClass` = `AllowableClass` + 4294967296
WHERE @legacy_item_class_mask AND `AllowableClass` BETWEEN -2147483648 AND -2;
