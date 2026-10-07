def leap(y): return y % 400 == 0 or (y % 4 == 0 and y % 100 != 0)
for y in [1900, 2000, 2024, 2023, 2100]:
    feb = 29 if leap(y) else 28
    print(f"{y}-03-01 leap={'true' if leap(y) else 'false'} feb={feb} doy={31 + feb + 1}")
