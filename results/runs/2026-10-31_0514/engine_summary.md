### data/skewed_1000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,745.2 (3,738–3,765) | 3,529.7 (3,494–3,537) | 1.06 |
| memory after the load (bytes/row) | 81.0 (81–81) | 115.2 (115–115) | 0.70 |
| open from the log (ms) | 425.7 (426–426) | 643.1 (642–643) | 0.66 |
| open from a snapshot (ms) | 108.8 (108–110) | 189.0 (189–189) | 0.58 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 548.5 (548–550) | 583.9 (582–588) | 0.94 |
| point lookup for a missing id (ns) | 173.9 (170–174) | 403.4 (398–404) | 0.43 |
| point lookup, parsed each time (ns) | 1,273.2 (1,247–1,321) | 1,294.5 (1,290–1,306) | 0.98 |
| of which the store's own lookup (ns) | 279.4 (277–286) | 314.3 (311–317) | 0.89 |
| range scan of 10 rows (ns) | 1,314.8 (1,216–1,316) | 1,326.0 (1,315–1,353) | 0.99 |
| range scan of 100 rows (ns) | 9,464.2 (9,423–9,538) | 9,392.4 (9,390–9,564) | 1.01 |
| range scan of 1000 rows (ns) | 81,886.7 (81,883–83,134) | 83,727.7 (83,576–84,374) | 0.98 |
| insert, random order (ns) | 3,622.0 (3,600–3,648) | 3,593.2 (3,590–3,608) | 1.01 |
| insert, appended past the end (ns) | 4,065.0 (4,064–4,072) | 2,740.3 (2,721–2,760) | 1.48 |
| 95% reads, 5% inserts following the data (ns/op) | 680.1 (678–694) | 785.8 (778–796) | 0.87 |
| 50% reads, 50% inserts following the data (ns/op) | 1,726.0 (1,723–1,736) | 1,724.8 (1,717–1,731) | 1.00 |
| 50% reads, 50% inserts spread evenly (ns/op) | 2,448.5 (2,448–2,450) | 1,561.8 (1,560–1,572) | 1.57 |
| durable insert, fsync every 1 (ns) | 2,687,445.6 (2,682,244–2,694,974) | 2,689,300.7 (2,686,182–2,692,967) | 1.00 |
| durable insert, fsync every 8 (ns) | 342,200.1 (341,828–343,049) | 342,593.6 (341,720–342,878) | 1.00 |
| durable insert, fsync every 64 (ns) | 47,193.0 (47,116–47,799) | 46,943.3 (46,859–47,001) | 1.01 |
| durable insert, fsync every 512 (ns) | 9,047.9 (9,027–9,087) | 8,748.0 (8,714–8,770) | 1.03 |

### data/sosd_books_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 4,026.5 | 3,941.5 | 1.02 |
| memory after the load (bytes/row) | 86.3 | 115.2 | 0.75 |
| open from the log (ms) | 6,077.5 | 8,420.8 | 0.72 |
| open from a snapshot (ms) | 1,461.6 | 2,187.5 | 0.67 |
| memory after opening the snapshot (bytes/row) | 89.2 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 733.7 | 860.1 | 0.85 |
| point lookup for a missing id (ns) | 318.0 | 687.7 | 0.46 |
| point lookup, parsed each time (ns) | 1,480.4 | 1,561.4 | 0.95 |
| of which the store's own lookup (ns) | 370.9 | 577.4 | 0.64 |
| range scan of 10 rows (ns) | 1,625.4 | 1,639.7 | 0.99 |
| range scan of 100 rows (ns) | 9,855.5 | 9,905.3 | 0.99 |
| range scan of 1000 rows (ns) | 83,153.7 | 84,252.7 | 0.99 |
| insert, random order (ns) | 3,830.1 | 3,953.6 | 0.97 |
| insert, appended past the end (ns) | 6,313.6 | 2,725.6 | 2.32 |
| 95% reads, 5% inserts following the data (ns/op) | 892.9 | 1,112.0 | 0.80 |
| 50% reads, 50% inserts following the data (ns/op) | 1,948.6 | 2,136.6 | 0.91 |
| 50% reads, 50% inserts spread evenly (ns/op) | 1,889.2 | 2,167.9 | 0.87 |
| durable insert, fsync every 1 (ns) | 2,685,346.9 | 2,696,380.0 | 1.00 |
| durable insert, fsync every 8 (ns) | 342,208.5 | 342,493.0 | 1.00 |
| durable insert, fsync every 64 (ns) | 47,208.0 | 46,986.9 | 1.00 |
| durable insert, fsync every 512 (ns) | 9,145.0 | 8,705.9 | 1.05 |

### data/sosd_fb_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,822.4 | 3,866.7 | 0.99 |
| memory after the load (bytes/row) | 88.4 | 115.2 | 0.77 |
| open from the log (ms) | 6,029.3 | 8,220.8 | 0.73 |
| open from a snapshot (ms) | 1,339.1 | 2,199.3 | 0.61 |
| memory after opening the snapshot (bytes/row) | 89.3 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 849.3 | 867.5 | 0.98 |
| point lookup for a missing id (ns) | 398.9 | 691.9 | 0.58 |
| point lookup, parsed each time (ns) | 1,651.4 | 1,580.0 | 1.05 |
| of which the store's own lookup (ns) | 460.9 | 591.0 | 0.78 |
| range scan of 10 rows (ns) | 1,749.0 | 1,666.4 | 1.05 |
| range scan of 100 rows (ns) | 10,243.4 | 9,897.3 | 1.03 |
| range scan of 1000 rows (ns) | 84,194.3 | 83,537.4 | 1.01 |
| insert, random order (ns) | 3,506.3 | 3,992.5 | 0.88 |
| insert, appended past the end (ns) | 6,439.2 | 2,742.3 | 2.35 |
| 95% reads, 5% inserts following the data (ns/op) | 949.6 | 1,184.0 | 0.80 |
| 50% reads, 50% inserts following the data (ns/op) | 1,887.6 | 2,113.5 | 0.89 |
| 50% reads, 50% inserts spread evenly (ns/op) | 4,285.9 | 1,803.2 | 2.38 |
| durable insert, fsync every 1 (ns) | 2,689,410.0 | 2,697,418.7 | 1.00 |
| durable insert, fsync every 8 (ns) | 342,849.9 | 343,106.3 | 1.00 |
| durable insert, fsync every 64 (ns) | 46,943.5 | 47,054.4 | 1.00 |
| durable insert, fsync every 512 (ns) | 8,762.5 | 8,717.4 | 1.01 |

### data/sosd_osm_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,687.8 | 3,966.0 | 0.93 |
| memory after the load (bytes/row) | 84.1 | 115.2 | 0.73 |
| open from the log (ms) | 6,173.1 | 8,310.6 | 0.74 |
| open from a snapshot (ms) | 1,464.2 | 2,244.5 | 0.65 |
| memory after opening the snapshot (bytes/row) | 89.3 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 840.1 | 871.6 | 0.96 |
| point lookup for a missing id (ns) | 396.2 | 692.7 | 0.57 |
| point lookup, parsed each time (ns) | 1,655.6 | 1,586.5 | 1.04 |
| of which the store's own lookup (ns) | 458.3 | 593.0 | 0.77 |
| range scan of 10 rows (ns) | 1,754.3 | 1,662.8 | 1.06 |
| range scan of 100 rows (ns) | 10,115.3 | 10,061.7 | 1.01 |
| range scan of 1000 rows (ns) | 84,769.5 | 87,523.2 | 0.97 |
| insert, random order (ns) | 3,538.3 | 4,032.5 | 0.88 |
| insert, appended past the end (ns) | 6,364.1 | 2,751.7 | 2.31 |
| 95% reads, 5% inserts following the data (ns/op) | 994.0 | 1,244.0 | 0.80 |
| 50% reads, 50% inserts following the data (ns/op) | 1,890.2 | 2,208.9 | 0.86 |
| 50% reads, 50% inserts spread evenly (ns/op) | 2,373.8 | 2,017.7 | 1.18 |
| durable insert, fsync every 1 (ns) | 2,694,585.0 | 2,692,567.3 | 1.00 |
| durable insert, fsync every 8 (ns) | 342,511.6 | 342,964.7 | 1.00 |
| durable insert, fsync every 64 (ns) | 46,952.2 | 46,987.4 | 1.00 |
| durable insert, fsync every 512 (ns) | 8,720.5 | 8,715.3 | 1.00 |

### data/sosd_wiki_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,895.0 | 3,998.2 | 0.97 |
| memory after the load (bytes/row) | 83.5 | 115.2 | 0.72 |
| open from the log (ms) | 6,082.1 | 8,409.5 | 0.72 |
| open from a snapshot (ms) | 1,457.5 | 2,202.3 | 0.66 |
| memory after opening the snapshot (bytes/row) | 89.2 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 784.9 | 884.2 | 0.89 |
| point lookup for a missing id (ns) | 354.3 | 713.0 | 0.50 |
| point lookup, parsed each time (ns) | 1,485.7 | 1,589.2 | 0.93 |
| of which the store's own lookup (ns) | 453.9 | 577.0 | 0.79 |
| range scan of 10 rows (ns) | 1,703.6 | 1,653.4 | 1.03 |
| range scan of 100 rows (ns) | 9,857.4 | 10,053.8 | 0.98 |
| range scan of 1000 rows (ns) | 82,356.0 | 86,408.7 | 0.95 |
| insert, random order (ns) | 3,768.7 | 4,039.0 | 0.93 |
| insert, appended past the end (ns) | 6,356.6 | 2,758.5 | 2.30 |
| 95% reads, 5% inserts following the data (ns/op) | 956.7 | 1,134.1 | 0.84 |
| 50% reads, 50% inserts following the data (ns/op) | 2,000.5 | 2,182.4 | 0.92 |
| 50% reads, 50% inserts spread evenly (ns/op) | 3,025.4 | 1,858.0 | 1.63 |
| durable insert, fsync every 1 (ns) | 2,684,607.5 | 2,683,513.1 | 1.00 |
| durable insert, fsync every 8 (ns) | 341,507.0 | 341,849.3 | 1.00 |
| durable insert, fsync every 64 (ns) | 46,987.9 | 46,955.3 | 1.00 |
| durable insert, fsync every 512 (ns) | 8,793.8 | 8,851.9 | 0.99 |

### data/uniform_10000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 4,036.2 (4,012–4,042) | 3,869.0 (3,838–3,896) | 1.04 |
| memory after the load (bytes/row) | 85.2 (85–85) | 115.2 (115–115) | 0.74 |
| open from the log (ms) | 6,100.2 (6,037–6,123) | 8,209.6 (8,017–8,304) | 0.74 |
| open from a snapshot (ms) | 1,464.5 (1,458–1,470) | 2,122.2 (2,112–2,138) | 0.69 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 747.7 (740–763) | 859.2 (857–913) | 0.87 |
| point lookup for a missing id (ns) | 327.5 (326–329) | 684.8 (678–743) | 0.48 |
| point lookup, parsed each time (ns) | 1,526.2 (1,473–1,535) | 1,563.7 (1,561–1,637) | 0.98 |
| of which the store's own lookup (ns) | 397.6 (391–398) | 570.5 (564–672) | 0.70 |
| range scan of 10 rows (ns) | 1,641.4 (1,624–1,677) | 1,624.2 (1,616–1,692) | 1.01 |
| range scan of 100 rows (ns) | 9,778.5 (9,755–9,860) | 9,996.3 (9,896–10,049) | 0.98 |
| range scan of 1000 rows (ns) | 82,657.8 (82,495–83,045) | 85,239.7 (84,753–85,502) | 0.97 |
| insert, random order (ns) | 3,852.1 (3,824–3,861) | 3,976.9 (3,952–4,002) | 0.97 |
| insert, appended past the end (ns) | 6,305.1 (6,304–6,309) | 2,735.7 (2,723–2,737) | 2.30 |
| 95% reads, 5% inserts following the data (ns/op) | 911.3 (904–922) | 1,175.5 (1,157–1,206) | 0.78 |
| 50% reads, 50% inserts following the data (ns/op) | 1,961.4 (1,953–1,981) | 2,132.0 (2,106–2,164) | 0.92 |
| 50% reads, 50% inserts spread evenly (ns/op) | 1,902.8 (1,902–1,943) | 2,105.3 (2,082–2,191) | 0.90 |
| durable insert, fsync every 1 (ns) | 2,694,454.0 (2,690,806–2,696,606) | 2,692,404.4 (2,689,129–2,692,725) | 1.00 |
| durable insert, fsync every 8 (ns) | 342,784.9 (342,547–343,225) | 343,025.1 (342,171–343,184) | 1.00 |
| durable insert, fsync every 64 (ns) | 47,305.2 (47,295–47,336) | 46,969.5 (46,872–47,239) | 1.01 |
| durable insert, fsync every 512 (ns) | 9,159.6 (9,119–9,167) | 8,699.4 (8,692–8,807) | 1.05 |

### data/uniform_1000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,755.4 (3,719–3,763) | 3,480.4 (3,476–3,497) | 1.08 |
| memory after the load (bytes/row) | 84.9 (85–85) | 115.2 (115–115) | 0.74 |
| open from the log (ms) | 421.5 (419–425) | 642.6 (641–647) | 0.66 |
| open from a snapshot (ms) | 108.8 (108–109) | 189.2 (189–190) | 0.58 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 545.9 (532–547) | 589.2 (589–591) | 0.93 |
| point lookup for a missing id (ns) | 166.2 (163–166) | 409.3 (409–409) | 0.41 |
| point lookup, parsed each time (ns) | 1,293.8 (1,214–1,303) | 1,284.7 (1,281–1,288) | 1.01 |
| of which the store's own lookup (ns) | 276.5 (274–277) | 326.5 (317–331) | 0.85 |
| range scan of 10 rows (ns) | 1,346.6 (1,301–1,349) | 1,360.8 (1,310–1,388) | 0.99 |
| range scan of 100 rows (ns) | 9,339.4 (9,313–9,416) | 9,566.7 (9,537–9,581) | 0.98 |
| range scan of 1000 rows (ns) | 82,163.3 (81,779–82,571) | 84,027.2 (83,475–84,912) | 0.98 |
| insert, random order (ns) | 3,574.2 (3,565–3,618) | 3,576.4 (3,520–3,609) | 1.00 |
| insert, appended past the end (ns) | 4,031.4 (4,029–4,076) | 2,697.7 (2,696–2,724) | 1.49 |
| 95% reads, 5% inserts following the data (ns/op) | 680.0 (666–683) | 794.1 (775–804) | 0.86 |
| 50% reads, 50% inserts following the data (ns/op) | 1,718.4 (1,717–1,743) | 1,759.4 (1,714–1,768) | 0.98 |
| 50% reads, 50% inserts spread evenly (ns/op) | 2,068.1 (2,050–2,080) | 1,775.9 (1,724–1,782) | 1.16 |
| durable insert, fsync every 1 (ns) | 2,696,859.9 (919,557–2,700,131) | 2,693,156.6 (2,688,928–2,696,148) | 1.00 |
| durable insert, fsync every 8 (ns) | 343,051.2 (219,591–343,153) | 342,907.1 (342,810–343,171) | 1.00 |
| durable insert, fsync every 64 (ns) | 47,413.6 (47,371–47,701) | 47,028.8 (47,009–47,078) | 1.01 |
| durable insert, fsync every 512 (ns) | 9,136.1 (9,128–9,178) | 8,756.5 (8,716–8,856) | 1.04 |

