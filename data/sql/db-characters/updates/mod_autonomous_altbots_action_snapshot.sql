-- mod-autonomous-altbots (Tofu fork): saved action bars (KeepActionBars).
-- Schema: acore_characters. Applied by the worldserver updater (state MODULE) at the next start; no reload command.
-- Re-runnable: CREATE TABLE IF NOT EXISTS only, so it never touches existing rows.
-- Check: SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'tofu_autoalt_action_snapshot'; -- expect 1

CREATE TABLE IF NOT EXISTS `tofu_autoalt_action_snapshot` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
    `spec` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `button` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `action` INT UNSIGNED NOT NULL DEFAULT 0,
    `type` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`, `spec`, `button`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-autonomous-altbots: action bars a real player left, restored when a real player logs in again';
