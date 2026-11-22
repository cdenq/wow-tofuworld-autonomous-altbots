-- mod-autonomous-altbots (Tofu fork): dungeons each autonomous alt party has cleared.
-- Schema: acore_characters. Applied by the worldserver updater (state MODULE) at the next start; no reload command.
-- Re-runnable: CREATE TABLE IF NOT EXISTS only, so it never touches existing rows.
-- Check: SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'tofu_autoalt_party_dungeon'; -- expect 1

CREATE TABLE IF NOT EXISTS `tofu_autoalt_party_dungeon` (
    `party_id` INT UNSIGNED NOT NULL COMMENT 'tofu_autoalt_party.id',
    `dungeon_id` INT UNSIGNED NOT NULL COMMENT 'LFGDungeons.dbc id',
    PRIMARY KEY (`party_id`, `dungeon_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-autonomous-altbots: dungeons a party has cleared';
