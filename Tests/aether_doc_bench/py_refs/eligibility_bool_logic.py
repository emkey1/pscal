# Tracked ambiguity (W1-11): the prompt does not say whether N in `guest N` is
# 0- or 1-based; this oracle written from the prompt alone chose 1-based. It
# follows the task's 0-based expected stdout until the prompt says so
# (py_refs/ambiguities.json).
def admit(age, t, v, f):
    return age >= 18 and (t or v) and not f
guests = [(20,True,False,False),(16,True,False,False),(25,False,False,False),(30,False,True,True),(40,True,False,False)]
for n, g in enumerate(guests):
    print(f"guest {n}: {'true' if admit(*g) else 'false'}")
