"""Dump the pilot table (0x43BA50 + id*0x38) of gundam.exe as JSON."""
import json, struct, sys
import disasm as D
SPEC = ['接近戦', '遠隔戦', '対艦戦']
SKILL = ['NOVICE', 'OFFICER', 'VETERAN', 'ACE', 'TOP ACE']
out = []
for i in range(130):
    b = D.read(0x43BA50 + i * 0x38, 0x38)
    name = b[:0x1C].split(b'\0')[0].decode('cp932')
    op, sh, ev, st, nt, cond, spec, grow, exp, status = struct.unpack_from('<10H', b, 0x1C)
    if not name:
        continue
    out.append(dict(id=i, name=name.replace(chr(92), ''), side='連邦' if i < 50 or 100 <= i < 115 else 'ジオン',
                    generic=i >= 100, 操縦=op, 射撃=sh, 回避=ev, スタミナ=st, NT=nt, CONDITION=cond,
                    SPEC=SPEC[spec] if spec < 3 else spec, 成長型=grow, EXP=exp, SKILL=SKILL[min(exp // 150, 4)],
                    status=status))
json.dump(out, sys.stdout, ensure_ascii=False, indent=0)
