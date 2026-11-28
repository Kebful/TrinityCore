ALTER TABLE `ffxiv_class_switch_classes`
    DROP CONSTRAINT `ffxiv_class_switch_id_range`,
    ADD CONSTRAINT `ffxiv_class_switch_id_range` CHECK (`class_id` BETWEEN 1 AND 64);
