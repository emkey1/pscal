import functools


@functools.lru_cache(maxsize=None)
def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


print(f"fib(30) = {fib(30)}")
print(f"fib(60) = {fib(60)}")
print(f"fib(90) = {fib(90)}")
print(f"sum = {sum(fib(i) for i in range(91))}")
