#!/usr/bin/env python3
import time
import asyncio

async def foo(n):
    if n > 1:
        await foo(n - 1)
    else:
        pass

async def bar(count, n):
    for _ in range(0, count):
        await foo(n)

async def main():
    count = 100_000_000
    for n in range(1, 6):
        start = time.time()
        await bar(count, n)
        end = time.time()
        elapsed_ns = (end - start) * 1000000000
        call_ns = int(elapsed_ns / count)
        print(f'Single call is {call_ns}ns for n={n}')

asyncio.run(main())
