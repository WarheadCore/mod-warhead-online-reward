DROP TABLE IF EXISTS `wh_online_rewards`;
CREATE TABLE `wh_online_rewards` (
  `ID` int(11) NOT NULL AUTO_INCREMENT,
  `IsPerOnline` tinyint(1) NOT NULL,
  `Seconds` int(20) NOT NULL DEFAULT 0,
  `Items` longtext CHARACTER SET utf8mb4 DEFAULT "",
  `Reputations` longtext CHARACTER SET utf8mb4 DEFAULT "",
  PRIMARY KEY (`ID`,`IsPerOnline`,`Seconds`) USING BTREE
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 ROW_FORMAT=COMPACT;

DROP TABLE IF EXISTS `wh_online_rewards_history`;
CREATE TABLE `wh_online_rewards_history` (
  `PlayerGuid` int(20) NOT NULL DEFAULT 0,
  `RewardID` int(11) NOT NULL DEFAULT 0,
  `RewardedSeconds` int(11) NOT NULL DEFAULT 0,
  PRIMARY KEY (`PlayerGuid`, `RewardID`) USING BTREE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=COMPACT;