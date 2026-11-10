-- Nameday and guardian joined the creation tuple (wire/tuple version 2), replacing the
-- separate ffxivcharacterprofile table. For databases that already applied
-- 2026_11_08_ffxiv_creation.sql; fresh databases get the columns from that file.
-- 0 = not chosen.
ALTER TABLE `ffxivcharactercustomize`
    ADD COLUMN IF NOT EXISTS `namedayMoon` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `flags`,
    ADD COLUMN IF NOT EXISTS `namedayDay` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `namedayMoon`,
    ADD COLUMN IF NOT EXISTS `guardian` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `namedayDay`;

DROP TABLE IF EXISTS `ffxivcharacterprofile`;
