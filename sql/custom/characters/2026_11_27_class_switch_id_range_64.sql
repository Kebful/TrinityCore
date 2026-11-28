ALTER TABLE `ffxiv_class_switch_profiles`
    DROP CONSTRAINT `ffxiv_class_switch_profile_id_range`,
    ADD CONSTRAINT `ffxiv_class_switch_profile_id_range` CHECK (`class_id` BETWEEN 1 AND 64);

ALTER TABLE `ffxiv_class_switch_progress`
    DROP CONSTRAINT `ffxiv_class_switch_progress_group_range`,
    ADD CONSTRAINT `ffxiv_class_switch_progress_group_range` CHECK (`exp_group` BETWEEN 1 AND 64);
