#!/usr/bin/env python3
"""Generate test archives for the Unveiler bridge test suite."""

import os
import sys
import zipfile
import struct

outdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/unveiler-test-archives"
os.makedirs(outdir, exist_ok=True)

# 1. Normal archive with files and directories
with zipfile.ZipFile(os.path.join(outdir, "normal.zip"), "w") as zf:
    zf.writestr("hello.txt", "hello world\n")
    zf.writestr("subdir/nested.txt", "nested content\n")
    zf.writestr("subdir/deep/file.txt", "deep content\n")
    zf.mkdir("emptydir/")

# 2. Path traversal attacks
with zipfile.ZipFile(os.path.join(outdir, "traversal.zip"), "w") as zf:
    zf.writestr("safe.txt", "safe\n")
    zf.writestr("../../etc/evil.txt", "pwned\n")
    zf.writestr("../escape.txt", "escaped\n")
    zf.writestr("/absolute.txt", "absolute\n")
    zf.writestr("foo/../../bar.txt", "sneaky\n")

# 3. Empty archive
with zipfile.ZipFile(os.path.join(outdir, "empty.zip"), "w") as zf:
    pass  # no entries

# 4. Archive with only directories
with zipfile.ZipFile(os.path.join(outdir, "dirs-only.zip"), "w") as zf:
    zf.mkdir("dir1/")
    zf.mkdir("dir1/dir2/")

# 5. Large filename (but valid)
with zipfile.ZipFile(os.path.join(outdir, "longname.zip"), "w") as zf:
    zf.writestr("a" * 200 + ".txt", "long filename\n")

# 6. Archive for overwrite policy testing
with zipfile.ZipFile(os.path.join(outdir, "overwrite.zip"), "w") as zf:
    zf.writestr("existing.txt", "new content\n")

# 7. Archive with dot-path entries
with zipfile.ZipFile(os.path.join(outdir, "dotpaths.zip"), "w") as zf:
    zf.writestr("./normal.txt", "dot-slash\n")
    zf.writestr("dir/./file.txt", "dot-in-middle\n")
    zf.writestr("safe.txt", "plain\n")

print(f"Generated test archives in {outdir}")
