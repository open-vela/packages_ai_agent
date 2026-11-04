#!/usr/bin/env python3
"""把设计稿 ui.json 烘焙成 C 字符串，编进固件。

真机没有可靠的文件系统，所以界面描述在编译期就变成一个大字符串常量。
设计师改完 ui.json 后跑一次这个脚本（或者由 tools/sync_design.sh 自动跑），
再编译烧录即可。

用法： python3 tools/embed_ui.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "ui", "assets", "ui.json")
SRC_THEME = os.path.join(HERE, "..", "src", "ui", "assets", "theme.ini")
SRC_TEXT = os.path.join(HERE, "..", "src", "ui", "assets", "text.ini")
OUT = os.path.join(HERE, "..", "src", "ui", "assets", "ui_json_embedded.c")

def main():
    if not os.path.exists(SRC):
        print("没有找到", SRC)
        return 1

    data = open(SRC, "rb").read().decode("utf-8")

    # 转成 C 字符串字面量：逐行拼接，避免超长行
    lines = []
    for ln in data.split("\n"):
        esc = ln.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")
        esc = esc.replace("\r", "")
        lines.append('    "%s\\n"' % esc)

    c = []
    c.append("/* 自动生成，勿手改。源文件：src/ui/assets/ui.json */")
    c.append("/* 生成器：tools/embed_ui.py */")
    c.append("")
    c.append('#include "ui_json_embedded.h"')
    c.append("")
    c.append("const char ui_json_embedded[] =")
    c.append("\n".join(lines) + ";")
    c.append("")
    c.append("const unsigned int ui_json_embedded_len = sizeof(ui_json_embedded) - 1;")
    c.append("")

    for path, name in ((SRC_THEME, "ui_theme_embedded"), (SRC_TEXT, "ui_text_embedded")):
        if not os.path.exists(path):
            c.append('const char %s[] = "";' % name)
            c.append("const unsigned int %s_len = 0;" % name)
            c.append("")
            continue
        txt = open(path, "rb").read().decode("utf-8")
        ls = []
        for ln in txt.split("\n"):
            esc = ln.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\r", "")
            ls.append('    "%s\\n"' % esc)
        c.append("const char %s[] =" % name)
        c.append("\n".join(ls) + ";")
        c.append("const unsigned int %s_len = sizeof(%s) - 1;" % (name, name))
        c.append("")

    open(OUT, "w", encoding="utf-8").write("\n".join(c))

    h = os.path.join(HERE, "..", "src", "ui", "assets", "ui_json_embedded.h")
    open(h, "w", encoding="utf-8").write(
        "#ifndef UI_JSON_EMBEDDED_H\n"
        "#define UI_JSON_EMBEDDED_H\n\n"
        "extern const char ui_json_embedded[];\n"
        "extern const unsigned int ui_json_embedded_len;\n\n"
        "#endif\n")

    print("生成 %s（%d 字节 JSON）" % (OUT, len(data.encode("utf-8"))))
    return 0

raise SystemExit(main())
