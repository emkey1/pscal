import sys


def parse_port(t):
    if not t or not all("0" <= ch <= "9" for ch in t):
        print(f"error: bad port {t}")
        sys.exit(2)
    return int(t)


for v in ["8080", "443", "80x", "22"]:
    print(f"port {parse_port(v)}")
