import io
s = io.StringIO()
s.write("telemetry frame 7")
body = s.getvalue()
s.close()
print(f"body = {body}")
print(f"len = {len(body)}")
