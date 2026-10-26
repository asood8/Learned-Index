### data/skewed_1000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 2,987.2 (2,975–3,003) | 2,619.3 (2,603–2,635) | 1.14 |
| memory after the load (bytes/row) | 81.0 (81–81) | 115.2 (115–115) | 0.70 |
| open from the log (ms) | 460.2 (446–494) | 784.7 (772–785) | 0.59 |
| open from a snapshot (ms) | 145.1 (142–162) | 220.5 (214–233) | 0.66 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 636.1 (614–650) | 666.0 (630–747) | 0.96 |
| point lookup for a missing id (ns) | 275.0 (236–289) | 602.6 (546–639) | 0.46 |
| point lookup, parsed each time (ns) | 1,516.4 (1,471–1,583) | 1,645.8 (1,594–1,657) | 0.92 |
| of which the store's own lookup (ns) | 419.6 (399–424) | 551.5 (488–570) | 0.76 |
| range scan of 10 rows (ns) | 1,306.3 (1,276–1,315) | 1,441.4 (1,377–1,500) | 0.91 |
| range scan of 100 rows (ns) | 10,892.3 (10,798–11,244) | 11,098.3 (10,985–11,462) | 0.98 |
| range scan of 1000 rows (ns) | 95,536.6 (90,510–96,166) | 94,858.5 (94,362–99,507) | 1.01 |
| insert, random order (ns) | 2,853.1 (2,757–2,966) | 2,732.5 (2,609–3,015) | 1.04 |
| insert, appended past the end (ns) | 3,413.5 (3,375–3,479) | 1,791.7 (1,745–1,951) | 1.91 |
| 95% reads, 5% inserts following the data (ns/op) | 763.6 (692–794) | 874.8 (853–920) | 0.87 |
| 50% reads, 50% inserts following the data (ns/op) | 1,326.5 (1,281–1,378) | 1,369.4 (1,226–1,442) | 0.97 |
| 50% reads, 50% inserts spread evenly (ns/op) | 2,255.3 (2,207–2,517) | 1,230.0 (1,067–1,289) | 1.83 |
| durable insert, fsync every 1 (ns) | 2,096,617.4 (1,845,619–2,199,091) | 2,073,707.2 (1,699,972–2,581,769) | 1.01 |
| durable insert, fsync every 8 (ns) | 240,687.4 (196,438–259,862) | 272,592.4 (236,779–276,438) | 0.88 |
| durable insert, fsync every 64 (ns) | 37,630.9 (27,588–47,194) | 40,063.7 (39,744–46,938) | 0.94 |
| durable insert, fsync every 512 (ns) | 7,717.0 (6,769–8,266) | 6,445.3 (6,282–7,712) | 1.20 |

### data/sosd_books_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,415.5 | 3,212.3 | 1.06 |
| memory after the load (bytes/row) | 86.3 | 115.2 | 0.75 |
| open from the log (ms) | 7,677.1 | 11,314.3 | 0.68 |
| open from a snapshot (ms) | 1,799.3 | 2,486.9 | 0.72 |
| memory after opening the snapshot (bytes/row) | 89.2 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 1,065.4 | 1,296.5 | 0.82 |
| point lookup for a missing id (ns) | 472.4 | 1,232.4 | 0.38 |
| point lookup, parsed each time (ns) | 1,787.2 | 1,922.4 | 0.93 |
| of which the store's own lookup (ns) | 676.0 | 966.5 | 0.70 |
| range scan of 10 rows (ns) | 1,527.5 | 1,657.9 | 0.92 |
| range scan of 100 rows (ns) | 11,382.5 | 11,743.4 | 0.97 |
| range scan of 1000 rows (ns) | 98,166.1 | 103,096.3 | 0.95 |
| insert, random order (ns) | 3,204.6 | 3,392.8 | 0.94 |
| insert, appended past the end (ns) | 6,135.3 | 1,741.2 | 3.52 |
| 95% reads, 5% inserts following the data (ns/op) | 1,266.0 | 1,853.0 | 0.68 |
| 50% reads, 50% inserts following the data (ns/op) | 1,767.4 | 1,971.2 | 0.90 |
| 50% reads, 50% inserts spread evenly (ns/op) | 1,777.5 | 1,990.9 | 0.89 |
| durable insert, fsync every 1 (ns) | 2,073,919.7 | 2,559,427.8 | 0.81 |
| durable insert, fsync every 8 (ns) | 225,895.3 | 256,411.5 | 0.88 |
| durable insert, fsync every 64 (ns) | 39,419.8 | 49,014.7 | 0.80 |
| durable insert, fsync every 512 (ns) | 7,705.2 | 8,661.2 | 0.89 |

### data/sosd_fb_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,125.1 | 3,242.2 | 0.96 |
| memory after the load (bytes/row) | 88.4 | 115.2 | 0.77 |
| open from the log (ms) | 7,454.7 | 11,560.1 | 0.64 |
| open from a snapshot (ms) | 1,692.3 | 2,577.2 | 0.66 |
| memory after opening the snapshot (bytes/row) | 89.3 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 1,139.6 | 1,231.3 | 0.93 |
| point lookup for a missing id (ns) | 649.3 | 1,140.8 | 0.57 |
| point lookup, parsed each time (ns) | 2,124.3 | 1,928.2 | 1.10 |
| of which the store's own lookup (ns) | 788.7 | 1,050.4 | 0.75 |
| range scan of 10 rows (ns) | 1,787.3 | 1,676.7 | 1.07 |
| range scan of 100 rows (ns) | 11,688.7 | 11,917.7 | 0.98 |
| range scan of 1000 rows (ns) | 92,461.4 | 98,313.1 | 0.94 |
| insert, random order (ns) | 2,635.1 | 3,299.8 | 0.80 |
| insert, appended past the end (ns) | 6,060.5 | 1,871.9 | 3.24 |
| 95% reads, 5% inserts following the data (ns/op) | 1,618.1 | 1,318.2 | 1.23 |
| 50% reads, 50% inserts following the data (ns/op) | 1,662.4 | 2,039.9 | 0.81 |
| 50% reads, 50% inserts spread evenly (ns/op) | 4,761.3 | 1,629.9 | 2.92 |
| durable insert, fsync every 1 (ns) | 2,495,533.1 | 1,552,681.4 | 1.61 |
| durable insert, fsync every 8 (ns) | 212,751.0 | 211,617.3 | 1.01 |
| durable insert, fsync every 64 (ns) | 31,974.2 | 35,556.7 | 0.90 |
| durable insert, fsync every 512 (ns) | 6,940.8 | 6,933.7 | 1.00 |

### data/sosd_osm_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 2,999.8 | 3,245.0 | 0.92 |
| memory after the load (bytes/row) | 84.1 | 115.2 | 0.73 |
| open from the log (ms) | 7,706.2 | 11,611.9 | 0.66 |
| open from a snapshot (ms) | 1,768.3 | 2,576.7 | 0.69 |
| memory after opening the snapshot (bytes/row) | 89.3 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 1,295.0 | 1,252.9 | 1.03 |
| point lookup for a missing id (ns) | 591.3 | 1,066.7 | 0.55 |
| point lookup, parsed each time (ns) | 1,919.6 | 1,875.5 | 1.02 |
| of which the store's own lookup (ns) | 800.1 | 896.0 | 0.89 |
| range scan of 10 rows (ns) | 1,739.4 | 1,976.4 | 0.88 |
| range scan of 100 rows (ns) | 11,763.0 | 11,947.6 | 0.98 |
| range scan of 1000 rows (ns) | 97,817.3 | 99,085.2 | 0.99 |
| insert, random order (ns) | 2,699.4 | 3,262.4 | 0.83 |
| insert, appended past the end (ns) | 6,205.2 | 1,725.2 | 3.60 |
| 95% reads, 5% inserts following the data (ns/op) | 1,511.9 | 1,586.9 | 0.95 |
| 50% reads, 50% inserts following the data (ns/op) | 1,989.1 | 2,057.0 | 0.97 |
| 50% reads, 50% inserts spread evenly (ns/op) | 2,340.8 | 1,595.8 | 1.47 |
| durable insert, fsync every 1 (ns) | 2,061,523.8 | 1,694,003.8 | 1.22 |
| durable insert, fsync every 8 (ns) | 268,758.1 | 231,872.1 | 1.16 |
| durable insert, fsync every 64 (ns) | 32,628.6 | 36,831.5 | 0.89 |
| durable insert, fsync every 512 (ns) | 5,375.6 | 7,609.5 | 0.71 |

### data/sosd_wiki_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,170.7 | 3,196.9 | 0.99 |
| memory after the load (bytes/row) | 83.5 | 115.2 | 0.72 |
| open from the log (ms) | 7,483.7 | 10,790.8 | 0.69 |
| open from a snapshot (ms) | 1,679.7 | 2,529.8 | 0.66 |
| memory after opening the snapshot (bytes/row) | 89.2 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 1,205.5 | 1,203.6 | 1.00 |
| point lookup for a missing id (ns) | 553.7 | 994.5 | 0.56 |
| point lookup, parsed each time (ns) | 2,039.0 | 1,856.3 | 1.10 |
| of which the store's own lookup (ns) | 750.7 | 1,098.8 | 0.68 |
| range scan of 10 rows (ns) | 1,695.9 | 1,643.4 | 1.03 |
| range scan of 100 rows (ns) | 11,973.5 | 12,148.4 | 0.99 |
| range scan of 1000 rows (ns) | 97,598.1 | 96,525.2 | 1.01 |
| insert, random order (ns) | 3,246.9 | 3,174.8 | 1.02 |
| insert, appended past the end (ns) | 6,000.2 | 1,722.4 | 3.48 |
| 95% reads, 5% inserts following the data (ns/op) | 1,047.2 | 1,376.5 | 0.76 |
| 50% reads, 50% inserts following the data (ns/op) | 1,810.2 | 1,809.5 | 1.00 |
| 50% reads, 50% inserts spread evenly (ns/op) | 3,670.6 | 1,620.5 | 2.27 |
| durable insert, fsync every 1 (ns) | 2,024,671.9 | 1,881,880.8 | 1.08 |
| durable insert, fsync every 8 (ns) | 280,653.5 | 247,073.4 | 1.14 |
| durable insert, fsync every 64 (ns) | 34,457.7 | 48,398.3 | 0.71 |
| durable insert, fsync every 512 (ns) | 7,080.6 | 6,684.7 | 1.06 |

### data/uniform_10000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,454.2 (3,418–3,525) | 3,223.8 (3,194–3,226) | 1.07 |
| memory after the load (bytes/row) | 85.2 (85–85) | 115.2 (115–115) | 0.74 |
| open from the log (ms) | 7,599.7 (7,516–7,913) | 11,505.1 (11,200–12,142) | 0.66 |
| open from a snapshot (ms) | 1,742.2 (1,712–1,885) | 2,487.3 (2,419–2,490) | 0.70 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 1,181.3 (1,028–1,205) | 1,247.1 (1,175–1,249) | 0.95 |
| point lookup for a missing id (ns) | 459.2 (456–462) | 1,092.0 (966–1,228) | 0.42 |
| point lookup, parsed each time (ns) | 1,839.2 (1,781–1,973) | 1,988.1 (1,809–1,989) | 0.93 |
| of which the store's own lookup (ns) | 717.9 (711–718) | 1,053.0 (902–1,070) | 0.68 |
| range scan of 10 rows (ns) | 2,044.0 (1,613–2,484) | 2,095.1 (1,660–2,158) | 0.98 |
| range scan of 100 rows (ns) | 11,439.9 (11,299–11,921) | 11,244.7 (11,232–11,556) | 1.02 |
| range scan of 1000 rows (ns) | 92,992.7 (92,795–96,234) | 96,247.8 (93,027–96,296) | 0.97 |
| insert, random order (ns) | 3,185.2 (3,129–3,225) | 3,268.5 (3,262–3,385) | 0.97 |
| insert, appended past the end (ns) | 6,072.6 (5,998–6,592) | 1,775.9 (1,760–1,957) | 3.42 |
| 95% reads, 5% inserts following the data (ns/op) | 1,206.9 (1,017–1,598) | 1,705.6 (1,274–2,155) | 0.71 |
| 50% reads, 50% inserts following the data (ns/op) | 1,618.0 (1,609–1,790) | 2,035.2 (1,815–2,157) | 0.80 |
| 50% reads, 50% inserts spread evenly (ns/op) | 1,610.4 (1,519–1,812) | 2,054.7 (2,009–2,127) | 0.78 |
| durable insert, fsync every 1 (ns) | 1,788,742.5 (1,588,065–2,168,559) | 2,580,518.8 (1,623,773–2,587,790) | 0.69 |
| durable insert, fsync every 8 (ns) | 193,536.3 (169,005–193,623) | 216,856.2 (168,850–327,574) | 0.89 |
| durable insert, fsync every 64 (ns) | 32,338.0 (30,316–34,368) | 34,149.9 (31,845–48,836) | 0.95 |
| durable insert, fsync every 512 (ns) | 7,793.6 (5,972–9,306) | 6,817.3 (6,689–9,210) | 1.14 |

### data/uniform_1000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 2,979.4 (2,952–2,990) | 2,578.5 (2,565–2,613) | 1.16 |
| memory after the load (bytes/row) | 84.9 (85–85) | 115.2 (115–115) | 0.74 |
| open from the log (ms) | 459.1 (432–481) | 705.4 (679–744) | 0.65 |
| open from a snapshot (ms) | 141.2 (134–152) | 220.2 (217–224) | 0.64 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 628.0 (597–640) | 684.4 (659–738) | 0.92 |
| point lookup for a missing id (ns) | 278.5 (270–284) | 605.6 (493–629) | 0.46 |
| point lookup, parsed each time (ns) | 1,470.3 (1,421–1,527) | 1,562.0 (1,523–1,666) | 0.94 |
| of which the store's own lookup (ns) | 355.6 (334–384) | 616.4 (612–666) | 0.58 |
| range scan of 10 rows (ns) | 1,381.8 (1,334–1,459) | 1,559.6 (1,365–2,022) | 0.89 |
| range scan of 100 rows (ns) | 11,025.6 (10,864–11,130) | 10,595.2 (10,570–11,231) | 1.04 |
| range scan of 1000 rows (ns) | 92,386.0 (92,031–96,568) | 93,386.8 (91,474–94,242) | 0.99 |
| insert, random order (ns) | 2,771.1 (2,673–3,014) | 2,680.0 (2,585–2,733) | 1.03 |
| insert, appended past the end (ns) | 3,438.9 (3,390–3,521) | 1,717.6 (1,685–1,739) | 2.00 |
| 95% reads, 5% inserts following the data (ns/op) | 693.8 (690–817) | 904.7 (819–913) | 0.77 |
| 50% reads, 50% inserts following the data (ns/op) | 1,314.0 (1,240–1,453) | 1,300.7 (1,293–1,341) | 1.01 |
| 50% reads, 50% inserts spread evenly (ns/op) | 1,822.7 (1,705–1,827) | 1,479.4 (1,376–1,718) | 1.23 |
| durable insert, fsync every 1 (ns) | 1,691,744.0 (1,392,490–2,537,108) | 1,770,899.1 (1,468,008–2,260,179) | 0.96 |
| durable insert, fsync every 8 (ns) | 233,871.6 (181,357–307,387) | 212,624.9 (186,518–223,427) | 1.10 |
| durable insert, fsync every 64 (ns) | 29,912.7 (29,713–33,527) | 32,168.6 (29,601–39,440) | 0.93 |
| durable insert, fsync every 512 (ns) | 7,102.9 (6,548–8,202) | 6,777.8 (5,972–8,322) | 1.05 |

