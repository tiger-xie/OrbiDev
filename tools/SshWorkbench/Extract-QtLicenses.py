"""从已校验的官方源码归档提取许可，不展开产品源文件。"""
import json
import pathlib
import re
import shutil
import sys
import tarfile

archive = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2]).resolve()
pattern = re.compile(r"^(LICENSE.*|LICENCE.*|COPYING.*|COPYRIGHT.*|NOTICE.*|AUTHORS.*|README\.chromium|qt_attribution\.json)$", re.I)
count = 0
index = []
with tarfile.open(archive, "r|xz") as source:
    for entry in source:
        path = pathlib.PurePosixPath(entry.name)
        if not entry.isfile() or not pattern.match(path.name):
            continue
        if path.is_absolute() or ".." in path.parts or ":" in entry.name or path.parts[0] != "qt-everywhere-src-5.15.2":
            raise ValueError("归档许可路径非法")
        # 扁平文件名避免 Chromium 的嵌套依赖超过 Windows 路径长度限制。
        destination = output / ("%05d.txt" % (count + 1))
        destination.parent.mkdir(parents=True, exist_ok=True)
        with source.extractfile(entry) as content, destination.open("wb") as target:
            shutil.copyfileobj(content, target)
        count += 1
        index.append({"file": destination.name, "sourcePath": entry.name})
if count < 100:
    raise ValueError("Qt 第三方许可集合异常")
(output / "index.json").write_text(json.dumps(index, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps({"count": count}))
