# mod-warhead-online-reward

## Table structure `wh_online_rewards`
1. `ID` - auto increment
2. `IsPerOnline` - Issuing an reward once or periodically
3. `Seconds` - Required amount of time online to receive an award (in seconds)
4. `Items` - Items for reward (itemid1[:count1],itemid2[:count2], ... itemidN[:countN])
5. `Reputations` - Reputations for reward (rep1[:count1],rep2[:count2] ... repN[:countN])

## How to
- For add rewards need using command `.or add`
```
.or add false 360 10 37711:1 71:5
.or add true 10 80 37711:5
```