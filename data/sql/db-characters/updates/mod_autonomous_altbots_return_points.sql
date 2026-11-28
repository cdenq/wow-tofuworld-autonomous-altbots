-- mod-autonomous-altbots (Tofu fork): where an alt goes back to after the guild hub (Hub.Escape).
-- Schema: acore_characters. Applied by the worldserver updater (state MODULE) at the next start; no reload command.
-- Re-runnable: CREATE TABLE IF NOT EXISTS only, so it never touches existing rows.
-- Check: SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'tofu_autoalt_return_point'; -- expect 1

CREATE TABLE IF NOT EXISTS `tofu_autoalt_return_point` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
    `map` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `x` FLOAT NOT NULL DEFAULT 0,
    `y` FLOAT NOT NULL DEFAULT 0,
    `z` FLOAT NOT NULL DEFAULT 0,
    `o` FLOAT NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-autonomous-altbots: open-world point an alt left for a hub trip or dungeon queue';
