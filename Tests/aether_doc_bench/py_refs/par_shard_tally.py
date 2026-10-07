def shard(lo, hi):
    t = c = 0
    for i in range(lo, hi):
        if i % 3 == 0:
            t += i; c += 1
    return t, c
a = shard(0, 25); b = shard(25, 50)
print(f"shardA total={a[0]} count={a[1]}")
print(f"shardB total={b[0]} count={b[1]}")
print(f"combined total={a[0]+b[0]} count={a[1]+b[1]}")
