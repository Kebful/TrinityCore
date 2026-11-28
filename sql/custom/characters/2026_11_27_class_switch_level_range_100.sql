-- Widen the class-switch level checks to the client's level ceiling (255).
--
-- The tables were created when classes capped at 80, and the CHECK constraints still say
-- 1..80. Any profile saved above 80 is rejected by MySQL (errno 4025,
-- `ffxiv_class_switch_profile_level_range`), which makes the character's save transaction fail
-- ("logout save failed; durable state remains at the last successful transaction") as soon as a
-- class switch or logout happens on a level-100 class.
--
-- MAX_CLASS_SWITCH_LEVEL in PlayerClassSwitch.h and MaxPlayerLevel are raised to 255 (the WoW
-- client's ceiling), so both constraints accept 1..255.

ALTER TABLE `ffxiv_class_switch_profiles`
    DROP CONSTRAINT `ffxiv_class_switch_profile_level_range`;

ALTER TABLE `ffxiv_class_switch_profiles`
    ADD CONSTRAINT `ffxiv_class_switch_profile_level_range`
        CHECK (`level` BETWEEN 1 AND 255);

ALTER TABLE `ffxiv_class_switch_progress`
    DROP CONSTRAINT `ffxiv_class_switch_progress_level_range`;

ALTER TABLE `ffxiv_class_switch_progress`
    ADD CONSTRAINT `ffxiv_class_switch_progress_level_range`
        CHECK (`level` BETWEEN 1 AND 255);
