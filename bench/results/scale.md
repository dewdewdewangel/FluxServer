# Worker-thread scaling — /stats endpoint

Fixed: wrk -t4 -c100 -d15s. Varies: server --threads

| --threads |     QPS    | P50 lat | P99 lat |
|----------:|-----------:|--------:|--------:|
|         1 |   70123.73 |  1.37ms |  2.38ms |
|         2 |  157038.99 | 578.00us | 406.90ms |
|         4 |  285238.12 | 271.00us |  2.87ms |
|         8 |  205764.46 | 351.00us | 386.63ms |
|        12 |  209825.69 | 358.00us |  6.72ms |
