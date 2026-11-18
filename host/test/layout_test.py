#!/usr/bin/env python3
"""The structs the loader and the kernel share are the same, field for field.

Two of them cross the boundary and neither side can include the other's header:

  struct vayu_ctl_mbox   bsd/sys/vayu_ctl.h  vs  xnuloader/loader.c
  struct loader_live     osfmk/arm64/loader_return.c (loader_live)
                         vs  xnuloader/loader.c (kernel_live)

The loader is a UEFI application: it reads both structs at a physical address and has its own
copy of each.  A field added on one side only would be silently at the wrong offset, so this
test takes the structs out of the sources, compiles them together for this machine and compares
every field's offset and size.

usage: python3 -I layout_test.py
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def find(rel, envvar):
    p = os.environ.get(envvar, "")
    if p and os.path.exists(p):
        return p
    for base in (os.path.join(HERE, "..", "..", "work"), os.path.join(HERE, "..", "..")):
        p = os.path.abspath(os.path.join(base, rel))
        if os.path.exists(p):
            return p
    raise SystemExit("layout_test: %s not found (set %s)" % (rel, envvar))


def struct_body(text, name):
    m = re.search(r"struct\s+" + name + r"\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise SystemExit("layout_test: struct %s not found" % name)
    return m.group(1)


def fields(body, what):
    out = []
    for line in body.splitlines():
        line = line.split("/*")[0].strip()
        if not line or line.startswith("*"):
            continue
        line = line.rstrip(";").strip()
        if not line:
            continue
        m = re.match(r"(?:volatile\s+)?(?:const\s+)?([A-Za-z_][A-Za-z0-9_]*)\s+(.*)$", line)
        if not m:
            raise SystemExit("%s: cannot read the field %r" % (what, line))
        for part in m.group(2).split(","):
            part = re.sub(r"^\*\s*", "", part.strip())
            part = re.sub(r"\[[^\]]*\]$", "", part.strip())
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", part):
                raise SystemExit("%s: cannot read the field %r" % (what, line))
            out.append(part)
    return out


def check(name, kernel_text, kernel_struct, loader_text, loader_struct, header=None):
    kf = fields(struct_body(kernel_text, kernel_struct), name + " (kernel)")
    lf = fields(struct_body(loader_text, loader_struct), name + " (loader)")
    print("== %s: %s (%d fields) vs %s" % (name, kernel_struct, len(kf), loader_struct))
    if kf != lf:
        print("FAIL the field lists differ\n  kernel: %s\n  loader: %s" % (kf, lf))
        return 1
    kc = kernel_struct if header else "kernel_copy"
    lc = "loader_copy"
    with tempfile.TemporaryDirectory(prefix="layout-") as tmp:
        src = os.path.join(tmp, "layout.c")
        with open(src, "w") as f:
            f.write("#include <stdio.h>\n#include <stddef.h>\n#include <stdint.h>\n#include <stdbool.h>\n")
            if header:
                f.write('#include "%s"\n' % header)
            else:
                f.write("struct %s {%s\n};\n" % (kc, struct_body(kernel_text, kernel_struct)))
            f.write("struct %s {%s\n};\n" % (lc, struct_body(loader_text, loader_struct)))
            f.write("int main(void) {\n")
            for field in kf:
                f.write('  printf("%%s %%zu %%zu %%zu %%zu\\n", "%s", '
                        "offsetof(struct %s, %s), sizeof(((struct %s *)0)->%s), "
                        "offsetof(struct %s, %s), sizeof(((struct %s *)0)->%s));\n"
                        % (field, kc, field, kc, field, lc, field, lc, field))
            f.write('  printf("size %%zu %%zu\\n", sizeof(struct %s), sizeof(struct %s));\n' % (kc, lc))
            f.write("  return 0;\n}\n")
        exe = os.path.join(tmp, "layout")
        subprocess.run([os.environ.get("CC", "cc"), "-O0", "-o", exe, src], check=True)
        out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout

    bad = 0
    for line in out.splitlines():
        parts = line.split()
        if parts[0] == "size":
            k, l = int(parts[1]), int(parts[2])
            print("  %-10s kernel %d loader %d %s" % ("sizeof", k, l, "OK" if k == l else "DIFF"))
            bad += k != l
            continue
        field, ko, ks, lo, ls = parts[0], int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
        ok = ko == lo and ks == ls
        bad += not ok
        print("  %-10s offset %3d/%3d  size %2d/%2d  %s" % (field, ko, lo, ks, ls, "OK" if ok else "DIFF"))
    print("  %s" % ("FAIL %d field(s) differ" % bad if bad else "PASS"))
    return bad


def main():
    header = find("src/bsd/sys/vayu_ctl.h", "VAYU_CTL_H")
    runtime = find("src/osfmk/arm64/loader_return.c", "LOADER_RETURN_C")
    loader = find("xnuloader/loader.c", "XNULOADER_C")
    bad = 0
    bad += check("vayu_ctl_mbox", open(header).read(), "vayu_ctl_mbox", open(loader).read(), "vayu_ctl_mbox",
                 header=header)
    bad += check("loader_live", open(runtime).read(), "loader_live", open(loader).read(), "kernel_live")
    print("PASS both structs are the same" if not bad else "FAIL %d field(s)" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
