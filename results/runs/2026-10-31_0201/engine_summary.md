### data/skewed_1000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 2,752.4 (2,704–2,778) | 2,436.3 (2,429–2,479) | 1.13 |
| memory after the load (bytes/row) | 81.0 (81–81) | 115.2 (115–115) | 0.70 |
| open from the log (ms) | 395.3 (390–430) | 620.6 (609–642) | 0.64 |
| open from a snapshot (ms) | 124.4 (124–124) | 205.4 (198–208) | 0.61 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 555.8 (556–556) | 608.4 (602–623) | 0.91 |
| point lookup for a missing id (ns) | 219.0 (216–221) | 495.1 (481–545) | 0.44 |
| point lookup, parsed each time (ns) | 1,320.3 (1,304–1,332) | 1,400.6 (1,396–1,474) | 0.94 |
| of which the store's own lookup (ns) | 253.9 (251–254) | 406.2 (405–406) | 0.63 |
| range scan of 10 rows (ns) | 1,226.4 (1,208–1,232) | 1,315.6 (1,306–1,321) | 0.93 |
| range scan of 100 rows (ns) | 10,323.2 (10,187–10,342) | 10,114.7 (10,103–10,225) | 1.02 |
| range scan of 1000 rows (ns) | 87,004.7 (86,151–87,287) | 88,618.6 (88,292–89,064) | 0.98 |
| insert, random order (ns) | 2,591.3 (2,591–2,658) | 2,511.8 (2,493–2,616) | 1.03 |
| insert, appended past the end (ns) | 3,191.7 (3,181–3,212) | 1,701.2 (1,693–1,711) | 1.88 |
| 95% reads, 5% inserts following the data (ns/op) | 650.0 (645–654) | 759.7 (744–819) | 0.86 |
| 50% reads, 50% inserts following the data (ns/op) | 1,219.2 (1,186–1,244) | 1,212.2 (1,202–1,223) | 1.01 |
| 50% reads, 50% inserts spread evenly (ns/op) | 2,040.9 (2,039–2,063) | 1,054.9 (1,020–1,057) | 1.93 |
| durable insert, fsync every 1 (ns) | 2,539,828.4 (1,643,718–2,654,175) | 2,380,499.7 (2,104,574–2,424,812) | 1.07 |
| durable insert, fsync every 8 (ns) | 329,052.0 (166,057–339,204) | 240,034.4 (188,878–286,268) | 1.37 |
| durable insert, fsync every 64 (ns) | 35,296.9 (25,644–43,502) | 44,259.6 (25,372–47,061) | 0.80 |
| durable insert, fsync every 512 (ns) | 9,185.7 (8,795–9,630) | 8,431.7 (7,867–8,776) | 1.09 |

### data/sosd_books_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,171.5 | 3,042.9 | 1.04 |
| memory after the load (bytes/row) | 86.3 | 115.2 | 0.75 |
| open from the log (ms) | 6,945.8 | 10,151.5 | 0.68 |
| open from a snapshot (ms) | 1,588.1 | 2,334.6 | 0.68 |
| memory after opening the snapshot (bytes/row) | 89.2 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 893.1 | 1,070.1 | 0.83 |
| point lookup for a missing id (ns) | 418.8 | 846.4 | 0.49 |
| point lookup, parsed each time (ns) | 1,641.0 | 1,645.7 | 1.00 |
| of which the store's own lookup (ns) | 502.6 | 860.6 | 0.58 |
| range scan of 10 rows (ns) | 1,479.5 | 1,674.6 | 0.88 |
| range scan of 100 rows (ns) | 10,445.4 | 10,496.6 | 1.00 |
| range scan of 1000 rows (ns) | 86,971.0 | 88,596.6 | 0.98 |
| insert, random order (ns) | 2,832.7 | 2,919.7 | 0.97 |
| insert, appended past the end (ns) | 5,666.0 | 1,738.8 | 3.26 |
| 95% reads, 5% inserts following the data (ns/op) | 957.2 | 1,521.1 | 0.63 |
| 50% reads, 50% inserts following the data (ns/op) | 1,390.0 | 1,665.1 | 0.83 |
| 50% reads, 50% inserts spread evenly (ns/op) | 1,408.3 | 1,718.0 | 0.82 |
| durable insert, fsync every 1 (ns) | 2,208,543.7 | 1,794,619.3 | 1.23 |
| durable insert, fsync every 8 (ns) | 249,509.2 | 220,032.8 | 1.13 |
| durable insert, fsync every 64 (ns) | 51,330.5 | 25,485.7 | 2.01 |
| durable insert, fsync every 512 (ns) | 8,244.7 | 5,459.3 | 1.51 |

### data/sosd_fb_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 2,870.4 | 2,988.0 | 0.96 |
| memory after the load (bytes/row) | 88.4 | 115.2 | 0.77 |
| open from the log (ms) | 6,682.7 | 10,038.3 | 0.67 |
| open from a snapshot (ms) | 1,517.4 | 2,270.3 | 0.67 |
| memory after opening the snapshot (bytes/row) | 89.3 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 1,036.9 | 1,029.7 | 1.01 |
| point lookup for a missing id (ns) | 491.9 | 872.2 | 0.56 |
| point lookup, parsed each time (ns) | 1,768.5 | 1,743.4 | 1.01 |
| of which the store's own lookup (ns) | 730.5 | 677.0 | 1.08 |
| range scan of 10 rows (ns) | 1,621.4 | 1,553.3 | 1.04 |
| range scan of 100 rows (ns) | 10,719.6 | 10,706.5 | 1.00 |
| range scan of 1000 rows (ns) | 86,491.9 | 91,851.7 | 0.94 |
| insert, random order (ns) | 2,418.4 | 2,945.7 | 0.82 |
| insert, appended past the end (ns) | 5,590.2 | 1,713.7 | 3.26 |
| 95% reads, 5% inserts following the data (ns/op) | 1,312.3 | 1,602.3 | 0.82 |
| 50% reads, 50% inserts following the data (ns/op) | 1,446.6 | 1,669.3 | 0.87 |
| 50% reads, 50% inserts spread evenly (ns/op) | 4,187.9 | 1,253.9 | 3.34 |
| durable insert, fsync every 1 (ns) | 1,802,547.8 | 2,216,726.0 | 0.81 |
| durable insert, fsync every 8 (ns) | 194,905.4 | 185,199.7 | 1.05 |
| durable insert, fsync every 64 (ns) | 35,963.0 | 27,554.3 | 1.31 |
| durable insert, fsync every 512 (ns) | 7,119.5 | 5,655.5 | 1.26 |

### data/sosd_osm_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 2,729.5 | 2,997.2 | 0.91 |
| memory after the load (bytes/row) | 84.1 | 115.2 | 0.73 |
| open from the log (ms) | 6,647.5 | 9,909.4 | 0.67 |
| open from a snapshot (ms) | 1,603.5 | 2,424.7 | 0.66 |
| memory after opening the snapshot (bytes/row) | 89.3 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 1,030.0 | 1,055.2 | 0.98 |
| point lookup for a missing id (ns) | 494.5 | 846.1 | 0.58 |
| point lookup, parsed each time (ns) | 1,809.7 | 1,631.1 | 1.11 |
| of which the store's own lookup (ns) | 500.5 | 675.8 | 0.74 |
| range scan of 10 rows (ns) | 1,701.4 | 1,557.5 | 1.09 |
| range scan of 100 rows (ns) | 10,710.7 | 10,865.6 | 0.99 |
| range scan of 1000 rows (ns) | 88,482.0 | 88,888.3 | 1.00 |
| insert, random order (ns) | 2,477.3 | 2,920.1 | 0.85 |
| insert, appended past the end (ns) | 5,756.9 | 1,705.7 | 3.38 |
| 95% reads, 5% inserts following the data (ns/op) | 1,116.6 | 1,364.4 | 0.82 |
| 50% reads, 50% inserts following the data (ns/op) | 1,391.1 | 1,667.8 | 0.83 |
| 50% reads, 50% inserts spread evenly (ns/op) | 2,197.0 | 1,415.2 | 1.55 |
| durable insert, fsync every 1 (ns) | 2,255,405.9 | 2,301,676.1 | 0.98 |
| durable insert, fsync every 8 (ns) | 220,131.7 | 324,213.2 | 0.68 |
| durable insert, fsync every 64 (ns) | 41,268.8 | 33,330.1 | 1.24 |
| durable insert, fsync every 512 (ns) | 7,950.7 | 5,242.2 | 1.52 |

### data/sosd_wiki_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 2,909.2 | 2,986.7 | 0.97 |
| memory after the load (bytes/row) | 83.5 | 115.2 | 0.72 |
| open from the log (ms) | 6,608.7 | 10,300.6 | 0.64 |
| open from a snapshot (ms) | 1,617.5 | 2,366.4 | 0.68 |
| memory after opening the snapshot (bytes/row) | 89.2 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 942.6 | 1,075.9 | 0.88 |
| point lookup for a missing id (ns) | 445.9 | 879.5 | 0.51 |
| point lookup, parsed each time (ns) | 1,721.2 | 1,740.3 | 0.99 |
| of which the store's own lookup (ns) | 469.8 | 1,014.4 | 0.46 |
| range scan of 10 rows (ns) | 1,592.2 | 1,593.6 | 1.00 |
| range scan of 100 rows (ns) | 10,801.1 | 10,691.8 | 1.01 |
| range scan of 1000 rows (ns) | 88,307.7 | 89,907.6 | 0.98 |
| insert, random order (ns) | 2,771.9 | 2,939.2 | 0.94 |
| insert, appended past the end (ns) | 5,432.7 | 1,736.0 | 3.13 |
| 95% reads, 5% inserts following the data (ns/op) | 1,249.9 | 1,503.7 | 0.83 |
| 50% reads, 50% inserts following the data (ns/op) | 1,493.8 | 1,812.8 | 0.82 |
| 50% reads, 50% inserts spread evenly (ns/op) | 2,902.8 | 1,318.5 | 2.20 |
| durable insert, fsync every 1 (ns) | 2,593,018.0 | 2,340,528.5 | 1.11 |
| durable insert, fsync every 8 (ns) | 251,398.5 | 312,051.9 | 0.81 |
| durable insert, fsync every 64 (ns) | 43,246.1 | 39,628.5 | 1.09 |
| durable insert, fsync every 512 (ns) | 8,131.0 | 5,667.7 | 1.43 |

### data/uniform_10000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,140.0 (3,131–3,225) | 3,004.0 (2,994–3,040) | 1.05 |
| memory after the load (bytes/row) | 85.2 (85–85) | 115.2 (115–115) | 0.74 |
| open from the log (ms) | 6,544.7 (6,376–6,555) | 9,689.5 (9,581–9,755) | 0.68 |
| open from a snapshot (ms) | 1,604.0 (1,602–1,632) | 2,279.8 (2,279–2,280) | 0.70 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 876.3 (864–912) | 1,055.4 (1,048–1,081) | 0.83 |
| point lookup for a missing id (ns) | 397.3 (396–399) | 880.4 (868–883) | 0.45 |
| point lookup, parsed each time (ns) | 1,585.3 (1,567–1,601) | 1,638.6 (1,631–1,732) | 0.97 |
| of which the store's own lookup (ns) | 471.7 (433–644) | 723.0 (715–748) | 0.65 |
| range scan of 10 rows (ns) | 1,566.4 (1,540–1,605) | 1,601.5 (1,592–1,654) | 0.98 |
| range scan of 100 rows (ns) | 10,616.7 (10,592–10,630) | 10,470.9 (10,412–10,476) | 1.01 |
| range scan of 1000 rows (ns) | 86,789.2 (86,365–88,121) | 88,238.1 (88,175–88,469) | 0.98 |
| insert, random order (ns) | 2,910.6 (2,907–2,911) | 2,916.6 (2,915–2,932) | 1.00 |
| insert, appended past the end (ns) | 5,652.3 (5,620–5,715) | 1,684.7 (1,683–1,706) | 3.36 |
| 95% reads, 5% inserts following the data (ns/op) | 964.7 (963–1,188) | 1,396.7 (1,355–1,446) | 0.69 |
| 50% reads, 50% inserts following the data (ns/op) | 1,490.6 (1,435–1,507) | 1,657.7 (1,636–1,694) | 0.90 |
| 50% reads, 50% inserts spread evenly (ns/op) | 1,443.2 (1,402–1,547) | 1,796.8 (1,696–1,880) | 0.80 |
| durable insert, fsync every 1 (ns) | 2,106,832.8 (1,771,609–2,482,142) | 2,321,222.0 (2,272,736–2,531,635) | 0.91 |
| durable insert, fsync every 8 (ns) | 213,433.0 (200,799–298,097) | 281,079.4 (253,849–316,347) | 0.76 |
| durable insert, fsync every 64 (ns) | 34,603.0 (32,462–34,794) | 42,342.3 (38,446–46,336) | 0.82 |
| durable insert, fsync every 512 (ns) | 6,520.3 (5,509–7,634) | 5,432.2 (5,399–6,265) | 1.20 |

### data/uniform_1000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 2,774.3 (2,733–2,806) | 2,450.5 (2,418–2,455) | 1.13 |
| memory after the load (bytes/row) | 84.9 (85–85) | 115.2 (115–115) | 0.74 |
| open from the log (ms) | 406.8 (399–419) | 624.4 (618–630) | 0.65 |
| open from a snapshot (ms) | 128.6 (124–130) | 200.8 (200–201) | 0.64 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 559.4 (539–576) | 601.8 (600–608) | 0.93 |
| point lookup for a missing id (ns) | 209.3 (208–224) | 483.1 (474–486) | 0.43 |
| point lookup, parsed each time (ns) | 1,366.3 (1,320–1,419) | 1,390.9 (1,373–1,421) | 0.98 |
| of which the store's own lookup (ns) | 305.8 (239–348) | 398.1 (397–399) | 0.77 |
| range scan of 10 rows (ns) | 1,222.8 (1,220–1,336) | 1,346.5 (1,318–1,352) | 0.91 |
| range scan of 100 rows (ns) | 10,404.7 (10,350–10,631) | 10,474.6 (10,325–10,503) | 0.99 |
| range scan of 1000 rows (ns) | 87,026.7 (86,572–88,346) | 89,352.6 (88,276–89,869) | 0.97 |
| insert, random order (ns) | 2,580.5 (2,561–2,594) | 2,483.7 (2,476–2,515) | 1.04 |
| insert, appended past the end (ns) | 3,186.5 (3,177–3,288) | 1,708.6 (1,679–1,725) | 1.86 |
| 95% reads, 5% inserts following the data (ns/op) | 650.6 (630–667) | 760.5 (742–773) | 0.86 |
| 50% reads, 50% inserts following the data (ns/op) | 1,192.7 (1,184–1,322) | 1,215.6 (1,199–1,220) | 0.98 |
| 50% reads, 50% inserts spread evenly (ns/op) | 1,592.1 (1,586–1,661) | 1,236.4 (1,230–1,238) | 1.29 |
| durable insert, fsync every 1 (ns) | 2,006,000.2 (1,867,979–2,042,732) | 2,858,974.8 (1,951,695–4,001,758) | 0.70 |
| durable insert, fsync every 8 (ns) | 201,133.2 (197,946–252,767) | 269,394.5 (188,792–277,369) | 0.75 |
| durable insert, fsync every 64 (ns) | 33,367.8 (32,195–39,106) | 47,660.7 (26,823–49,002) | 0.70 |
| durable insert, fsync every 512 (ns) | 8,594.1 (6,199–9,206) | 7,275.5 (5,514–8,959) | 1.18 |

