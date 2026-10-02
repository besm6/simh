import difflib

REAL = "adap_from_b6.bin"
NEW = "disk67_zone0-3.bin"
BASE = 0o30000


def load_words(path):
    with open(path, "rb") as f:
        data = f.read()
    return [int.from_bytes(data[i * 6:i * 6 + 6], "big") for i in range(len(data) // 6)]


def decode_syl(v):
    v &= 0xFFFFFF
    M = (v >> 20) & 0xF
    rem = v & 0xFFFFF
    if rem & 0x80000:
        return (2, M, ((rem >> 15) & 0xF) + 0o20, rem & 0x7FFF)
    return (1, M, (rem >> 12) & 0x3F, rem & 0xFFF)


def decode_word(w):
    return decode_syl(w >> 24), decode_syl(w)


def oaddr(a):
    return oct(a)[2:]


def main():
    real_words = load_words(REAL)
    new_words = load_words(NEW)[:len(real_words)]
    print(f"real: {len(real_words)} words, new(trunc): {len(new_words)} words")

    sm = difflib.SequenceMatcher(None, new_words, real_words, autojunk=False)
    len_mismatch, real_diff, drift = [], [], []

    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        if (i2 - i1) != (j2 - j1):
            len_mismatch.append((tag, i1, i2, j1, j2))
            continue
        for k in range(i2 - i1):
            nw, rw = new_words[i1 + k], real_words[j1 + k]
            if nw == rw:
                continue
            nd, rd = decode_word(nw), decode_word(rw)
            addr = BASE + j1 + k
            if (nd[0][:3], nd[1][:3]) == (rd[0][:3], rd[1][:3]):
                drift.append((addr, nd, rd))
            else:
                real_diff.append((addr, nd, rd))

    print(f"\nlen_mismatch: {len(len_mismatch)}")
    for tag, i1, i2, j1, j2 in len_mismatch:
        print(f"  {tag}: new[{oaddr(BASE+i1)}:{oaddr(BASE+i2)}] ({i2-i1}w) "
              f"<-> real[{oaddr(BASE+j1)}:{oaddr(BASE+j2)}] ({j2-j1}w)")

    print(f"\nreal_diff: {len(real_diff)}")
    for addr, nd, rd in real_diff:
        print(f"  {oaddr(addr)}: new={nd} real={rd}")

    print(f"\ndrift (benign, address-only): {len(drift)}")
    for addr, nd, rd in drift:
        print(f"  {oaddr(addr)}: new={nd} real={rd}")


if __name__ == "__main__":
    main()
