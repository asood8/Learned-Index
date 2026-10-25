### data/skewed_1000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 2,911.4 (2,907–2,992) | 2,564.0 (2,554–3,192) | 1.14 |
| memory after the load (bytes/row) | 81.0 (81–81) | 115.2 (115–115) | 0.70 |
| open from the log (ms) | 449.9 (417–465) | 705.2 (678–859) | 0.64 |
| open from a snapshot (ms) | 139.3 (130–167) | 221.1 (212–241) | 0.63 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 588.8 (588–671) | 692.5 (622–791) | 0.85 |
| point lookup for a missing id (ns) | 238.5 (230–260) | 513.9 (508–519) | 0.46 |
| point lookup, parsed each time (ns) | 1,453.4 (1,451–1,490) | 1,520.7 (1,516–1,530) | 0.96 |
| of which the store's own lookup (ns) | 319.2 (296–400) | 510.7 (416–532) | 0.63 |
| range scan of 10 rows (ns) | 1,346.5 (1,271–1,386) | 1,446.0 (1,331–1,589) | 0.93 |
| range scan of 100 rows (ns) | 10,839.6 (10,822–11,290) | 10,962.1 (10,768–11,212) | 0.99 |
| range scan of 1000 rows (ns) | 92,407.5 (92,300–96,269) | 94,149.6 (92,152–95,902) | 0.98 |
| insert, random order (ns) | 2,767.4 (2,722–2,951) | 2,597.1 (2,575–2,686) | 1.07 |
| insert, appended past the end (ns) | 3,380.5 (3,347–3,392) | 1,729.8 (1,717–1,768) | 1.95 |
| 95% reads, 5% inserts following the data (ns/op) | 706.2 (695–749) | 801.4 (796–1,007) | 0.88 |
| 50% reads, 50% inserts following the data (ns/op) | 1,459.9 (1,295–1,478) | 1,294.5 (1,293–1,300) | 1.13 |
| 50% reads, 50% inserts spread evenly (ns/op) | 2,391.9 (2,121–2,626) | 1,111.2 (1,058–1,127) | 2.15 |
| durable insert, fsync every 1 (ns) | 1,346,548.6 (1,243,760–1,798,855) | 1,390,628.3 (1,364,423–1,697,183) | 0.97 |
| durable insert, fsync every 8 (ns) | 167,367.7 (148,292–218,903) | 170,589.6 (166,876–188,512) | 0.98 |
| durable insert, fsync every 64 (ns) | 27,572.0 (20,470–30,799) | 31,270.6 (24,768–34,707) | 0.88 |
| durable insert, fsync every 512 (ns) | 7,675.4 (6,123–7,928) | 7,290.1 (6,482–7,718) | 1.05 |

### data/sosd_books_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,261.1 | 3,216.3 | 1.01 |
| memory after the load (bytes/row) | 86.3 | 115.2 | 0.75 |
| open from the log (ms) | 6,305.7 | 11,785.6 | 0.54 |
| open from a snapshot (ms) | 1,678.2 | 2,511.9 | 0.67 |
| memory after opening the snapshot (bytes/row) | 89.2 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 925.2 | 1,050.3 | 0.88 |
| point lookup for a missing id (ns) | 403.2 | 881.7 | 0.46 |
| point lookup, parsed each time (ns) | 1,719.5 | 1,863.5 | 0.92 |
| of which the store's own lookup (ns) | 688.1 | 737.0 | 0.93 |
| range scan of 10 rows (ns) | 1,552.4 | 1,891.0 | 0.82 |
| range scan of 100 rows (ns) | 11,555.3 | 11,251.9 | 1.03 |
| range scan of 1000 rows (ns) | 94,285.7 | 93,843.8 | 1.00 |
| insert, random order (ns) | 3,089.3 | 3,153.6 | 0.98 |
| insert, appended past the end (ns) | 5,652.0 | 1,787.8 | 3.16 |
| 95% reads, 5% inserts following the data (ns/op) | 1,191.7 | 1,461.2 | 0.82 |
| 50% reads, 50% inserts following the data (ns/op) | 1,533.7 | 2,097.6 | 0.73 |
| 50% reads, 50% inserts spread evenly (ns/op) | 1,794.4 | 1,815.5 | 0.99 |
| durable insert, fsync every 1 (ns) | 1,713,770.1 | 1,662,402.2 | 1.03 |
| durable insert, fsync every 8 (ns) | 182,463.0 | 179,962.3 | 1.01 |
| durable insert, fsync every 64 (ns) | 35,183.2 | 31,583.8 | 1.11 |
| durable insert, fsync every 512 (ns) | 7,729.1 | 8,003.4 | 0.97 |

### data/sosd_fb_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 2,967.3 | 3,138.2 | 0.95 |
| memory after the load (bytes/row) | 88.4 | 115.2 | 0.77 |
| open from the log (ms) | 7,053.1 | 11,819.7 | 0.60 |
| open from a snapshot (ms) | 1,557.7 | 2,458.1 | 0.63 |
| memory after opening the snapshot (bytes/row) | 89.3 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 1,261.4 | 1,082.0 | 1.17 |
| point lookup for a missing id (ns) | 464.5 | 846.5 | 0.55 |
| point lookup, parsed each time (ns) | 1,844.5 | 1,985.2 | 0.93 |
| of which the store's own lookup (ns) | 814.2 | 640.6 | 1.27 |
| range scan of 10 rows (ns) | 1,895.5 | 1,897.5 | 1.00 |
| range scan of 100 rows (ns) | 11,490.3 | 11,383.0 | 1.01 |
| range scan of 1000 rows (ns) | 94,660.1 | 95,734.8 | 0.99 |
| insert, random order (ns) | 2,579.7 | 3,157.7 | 0.82 |
| insert, appended past the end (ns) | 5,388.5 | 1,743.2 | 3.09 |
| 95% reads, 5% inserts following the data (ns/op) | 1,245.0 | 1,654.2 | 0.75 |
| 50% reads, 50% inserts following the data (ns/op) | 1,795.5 | 1,908.1 | 0.94 |
| 50% reads, 50% inserts spread evenly (ns/op) | 4,671.4 | 1,371.5 | 3.41 |
| durable insert, fsync every 1 (ns) | 1,340,229.9 | 1,645,037.3 | 0.81 |
| durable insert, fsync every 8 (ns) | 215,273.2 | 199,320.5 | 1.08 |
| durable insert, fsync every 64 (ns) | 33,297.3 | 25,955.1 | 1.28 |
| durable insert, fsync every 512 (ns) | 6,450.1 | 5,861.6 | 1.10 |

### data/sosd_osm_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 2,824.5 | 3,185.3 | 0.89 |
| memory after the load (bytes/row) | 84.1 | 115.2 | 0.73 |
| open from the log (ms) | 7,147.9 | 11,471.3 | 0.62 |
| open from a snapshot (ms) | 1,718.5 | 2,544.4 | 0.68 |
| memory after opening the snapshot (bytes/row) | 89.3 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 1,068.0 | 1,060.8 | 1.01 |
| point lookup for a missing id (ns) | 488.6 | 916.0 | 0.53 |
| point lookup, parsed each time (ns) | 2,019.1 | 2,001.2 | 1.01 |
| of which the store's own lookup (ns) | 730.4 | 788.3 | 0.93 |
| range scan of 10 rows (ns) | 1,858.8 | 1,676.8 | 1.11 |
| range scan of 100 rows (ns) | 11,603.2 | 11,267.0 | 1.03 |
| range scan of 1000 rows (ns) | 92,171.1 | 93,666.0 | 0.98 |
| insert, random order (ns) | 2,563.6 | 3,198.7 | 0.80 |
| insert, appended past the end (ns) | 5,543.7 | 1,775.5 | 3.12 |
| 95% reads, 5% inserts following the data (ns/op) | 1,313.7 | 1,583.8 | 0.83 |
| 50% reads, 50% inserts following the data (ns/op) | 1,491.1 | 1,937.3 | 0.77 |
| 50% reads, 50% inserts spread evenly (ns/op) | 2,321.5 | 1,638.9 | 1.42 |
| durable insert, fsync every 1 (ns) | 1,999,836.4 | 1,206,179.3 | 1.66 |
| durable insert, fsync every 8 (ns) | 207,540.2 | 173,415.1 | 1.20 |
| durable insert, fsync every 64 (ns) | 32,138.8 | 33,531.1 | 0.96 |
| durable insert, fsync every 512 (ns) | 5,888.3 | 7,309.4 | 0.81 |

### data/sosd_wiki_10000000.bin (1 run each)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,086.0 | 3,130.5 | 0.99 |
| memory after the load (bytes/row) | 83.5 | 115.2 | 0.72 |
| open from the log (ms) | 7,240.8 | 10,948.7 | 0.66 |
| open from a snapshot (ms) | 1,638.6 | 2,506.0 | 0.65 |
| memory after opening the snapshot (bytes/row) | 89.2 | 94.3 | 0.95 |
| point lookup, parsed ahead of time (ns) | 922.0 | 1,047.5 | 0.88 |
| point lookup for a missing id (ns) | 445.7 | 884.7 | 0.50 |
| point lookup, parsed each time (ns) | 1,778.3 | 1,836.1 | 0.97 |
| of which the store's own lookup (ns) | 441.2 | 925.4 | 0.48 |
| range scan of 10 rows (ns) | 1,635.2 | 1,733.4 | 0.94 |
| range scan of 100 rows (ns) | 11,002.1 | 11,356.2 | 0.97 |
| range scan of 1000 rows (ns) | 98,099.6 | 94,241.3 | 1.04 |
| insert, random order (ns) | 3,170.3 | 3,222.6 | 0.98 |
| insert, appended past the end (ns) | 5,585.9 | 1,755.6 | 3.18 |
| 95% reads, 5% inserts following the data (ns/op) | 1,108.6 | 1,794.1 | 0.62 |
| 50% reads, 50% inserts following the data (ns/op) | 1,657.9 | 2,331.4 | 0.71 |
| 50% reads, 50% inserts spread evenly (ns/op) | 3,239.4 | 1,500.0 | 2.16 |
| durable insert, fsync every 1 (ns) | 1,994,247.9 | 1,632,240.0 | 1.22 |
| durable insert, fsync every 8 (ns) | 203,277.8 | 183,268.5 | 1.11 |
| durable insert, fsync every 64 (ns) | 35,877.5 | 33,153.8 | 1.08 |
| durable insert, fsync every 512 (ns) | 7,802.6 | 5,598.6 | 1.39 |

### data/uniform_10000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,270.4 (3,254–3,384) | 3,173.0 (3,165–3,351) | 1.03 |
| memory after the load (bytes/row) | 85.2 (85–85) | 115.2 (115–115) | 0.74 |
| open from the log (ms) | 6,822.9 (6,312–7,552) | 12,097.1 (11,564–12,121) | 0.56 |
| open from a snapshot (ms) | 1,726.7 (1,695–1,848) | 2,420.3 (2,414–2,441) | 0.71 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 996.9 (894–1,203) | 1,149.0 (1,056–1,192) | 0.87 |
| point lookup for a missing id (ns) | 456.9 (430–478) | 1,055.1 (895–1,156) | 0.43 |
| point lookup, parsed each time (ns) | 1,772.9 (1,750–1,799) | 1,914.0 (1,802–2,014) | 0.93 |
| of which the store's own lookup (ns) | 745.7 (450–749) | 725.2 (689–794) | 1.03 |
| range scan of 10 rows (ns) | 1,646.0 (1,640–1,816) | 1,814.4 (1,706–1,911) | 0.91 |
| range scan of 100 rows (ns) | 11,465.1 (11,166–11,482) | 11,382.0 (10,836–11,520) | 1.01 |
| range scan of 1000 rows (ns) | 92,266.6 (91,552–95,268) | 93,088.2 (92,937–95,771) | 0.99 |
| insert, random order (ns) | 3,210.8 (3,048–3,218) | 3,178.2 (3,174–3,255) | 1.01 |
| insert, appended past the end (ns) | 5,503.6 (5,287–5,844) | 1,824.3 (1,813–1,844) | 3.02 |
| 95% reads, 5% inserts following the data (ns/op) | 1,036.8 (897–1,238) | 1,415.2 (1,337–1,498) | 0.73 |
| 50% reads, 50% inserts following the data (ns/op) | 1,636.1 (1,611–1,671) | 1,812.1 (1,692–1,940) | 0.90 |
| 50% reads, 50% inserts spread evenly (ns/op) | 1,667.9 (1,524–1,714) | 1,813.3 (1,789–1,837) | 0.92 |
| durable insert, fsync every 1 (ns) | 1,478,834.9 (1,207,034–1,793,592) | 1,869,389.9 (1,195,240–1,904,162) | 0.79 |
| durable insert, fsync every 8 (ns) | 213,555.3 (207,573–311,343) | 201,168.5 (146,163–204,649) | 1.06 |
| durable insert, fsync every 64 (ns) | 32,200.2 (27,254–33,580) | 30,768.1 (28,332–33,048) | 1.05 |
| durable insert, fsync every 512 (ns) | 7,325.5 (6,730–7,362) | 6,507.8 (5,458–6,935) | 1.13 |

### data/uniform_1000000.bin (median of 3 runs each, with the lowest and highest)

| | learned index | B+-tree (tlx) | learned / B+-tree |
|---|---|---|---|
| load, random order (ns/row) | 3,131.8 (2,854–3,246) | 2,649.3 (2,605–2,712) | 1.18 |
| memory after the load (bytes/row) | 84.9 (85–85) | 115.2 (115–115) | 0.74 |
| open from the log (ms) | 617.9 (440–693) | 790.3 (753–813) | 0.78 |
| open from a snapshot (ms) | 140.3 (137–177) | 217.3 (213–226) | 0.65 |
| memory after opening the snapshot (bytes/row) | 89.2 (89–89) | 94.3 (94–94) | 0.95 |
| point lookup, parsed ahead of time (ns) | 615.9 (587–652) | 691.9 (672–724) | 0.89 |
| point lookup for a missing id (ns) | 231.9 (230–248) | 534.2 (520–547) | 0.43 |
| point lookup, parsed each time (ns) | 1,404.9 (1,386–1,481) | 1,484.5 (1,483–1,616) | 0.95 |
| of which the store's own lookup (ns) | 288.9 (265–295) | 451.8 (442–455) | 0.64 |
| range scan of 10 rows (ns) | 1,367.9 (1,322–1,528) | 1,375.8 (1,369–1,415) | 0.99 |
| range scan of 100 rows (ns) | 10,995.6 (10,663–11,076) | 10,822.3 (10,419–11,039) | 1.02 |
| range scan of 1000 rows (ns) | 92,587.9 (90,628–94,408) | 95,339.5 (94,724–95,594) | 0.97 |
| insert, random order (ns) | 2,830.0 (2,750–2,875) | 2,625.3 (2,608–2,864) | 1.08 |
| insert, appended past the end (ns) | 3,419.2 (3,317–3,535) | 1,772.1 (1,727–1,775) | 1.93 |
| 95% reads, 5% inserts following the data (ns/op) | 681.8 (654–732) | 971.2 (853–984) | 0.70 |
| 50% reads, 50% inserts following the data (ns/op) | 1,321.7 (1,278–1,367) | 1,397.9 (1,325–1,435) | 0.95 |
| 50% reads, 50% inserts spread evenly (ns/op) | 1,781.2 (1,680–1,825) | 1,415.7 (1,332–1,423) | 1.26 |
| durable insert, fsync every 1 (ns) | 1,671,686.0 (1,175,049–1,741,514) | 1,665,430.0 (1,182,829–2,590,049) | 1.00 |
| durable insert, fsync every 8 (ns) | 204,363.6 (134,231–204,972) | 175,993.1 (145,344–178,049) | 1.16 |
| durable insert, fsync every 64 (ns) | 38,164.6 (21,647–42,437) | 30,453.6 (25,548–40,139) | 1.25 |
| durable insert, fsync every 512 (ns) | 7,623.9 (4,866–9,676) | 7,687.5 (7,485–7,940) | 0.99 |

