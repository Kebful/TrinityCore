DELETE profile
FROM `ffxiv_class_switch_profiles` AS profile
LEFT JOIN `characters` AS character_record
    ON character_record.guid = profile.guid
WHERE character_record.guid IS NULL;
