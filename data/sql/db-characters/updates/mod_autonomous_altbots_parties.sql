-- mod-autonomous-altbots (Tofu fork): autonomous alt parties.
-- Schema: acore_characters. Applied by the worldserver updater (state MODULE) at the next start; no reload command.
-- Re-runnable: CREATE TABLE IF NOT EXISTS only, so it never touches existing rows.
-- Check: SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN ('tofu_autoalt_party', 'tofu_autoalt_party_member'); -- expect 2

CREATE TABLE IF NOT EXISTS `tofu_autoalt_party` (
    `id` INT UNSIGNED NOT NULL,
    `name` VARCHAR(32) NOT NULL,
    `owner_account` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'account that created the party; 0 = console',
    PRIMARY KEY (`id`),
    UNIQUE KEY `name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-autonomous-altbots: autonomous alt parties';

CREATE TABLE IF NOT EXISTS `tofu_autoalt_party_member` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
    `party_id` INT UNSIGNED NOT NULL COMMENT 'tofu_autoalt_party.id',
    PRIMARY KEY (`guid`),
    KEY `party_id` (`party_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-autonomous-altbots: one row per party member';
