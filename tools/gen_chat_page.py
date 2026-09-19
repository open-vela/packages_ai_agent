#!/usr/bin/env python3
"""把 res/phone_chat.html 转成 src/infra/chat_page.h（C 字符串常量）。

两条硬规则：

1. **非 ASCII 用八进制转义**（\\NNN）而不是 \\xNN —— C 的 \\x 转义是贪婪的，
   后面紧跟十六进制字符会被一起吃掉；八进制最多三位，不会越界。
2. **按原始字节分片，再逐片转义**，而不是先转义再分片 —— 后者会把一个
   `\\342` 从中间切开（成 `\\34` + `2`），生成出非法字节。
   （v1 就是这么错的：`⏰` 变成 `1c 32 8f b0`，页面里那两个字变成乱码。）

生成后自校验：把写出的文件读回来解码，与源文件逐字节比对，不一致直接报错。

用法（在 packages/ai_agent 下）：
    python3 tools/gen_chat_page.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "res", "phone_chat.html")
DST = os.path.join(HERE, "..", "src", "infra", "chat_page.h")
NAME = "CHAT_PAGE_HTML"
GROUP = 96  # 每个 C 字符串片段对应的原始字节数


def escape_byte(b: int) -> str:
    if b == 0x22:
        return '\\"'
    if b == 0x5C:
        return "\\\\"
    if b == 0x0A:
        return "\\n"
    if b == 0x0D:
        return "\\r"
    if b == 0x09:
        return "\\t"
    if 0x20 <= b < 0x7F:
        return chr(b)
    return "\\%03o" % b


def main() -> int:
    data = open(SRC, "rb").read()
    groups = [data[i:i + GROUP] for i in range(0, len(data), GROUP)]

    lines = "".join('    "%s"\n' % "".join(escape_byte(b) for b in g)
                    for g in groups)

    header = (
        "/* chat_page.h - 手机对话网页（自动生成，请勿手改）\n"
        " *\n"
        " * 源文件：packages/ai_agent/res/phone_chat.html\n"
        " * 生成：python3 tools/gen_chat_page.py\n"
        " *\n"
        " * 手机浏览器打开 http://<设备IP>:28789/ 时由 api_handler 直接返回本页，\n"
        " * 页面自身再连同一端口的 WebSocket，因此不需要任何 App 或外网。\n"
        " */\n\n"
        "#ifndef AI_AGENT_CHAT_PAGE_H\n"
        "#define AI_AGENT_CHAT_PAGE_H\n\n"
        "static const char %s[] =\n" % NAME)
    with open(DST, "w", encoding="utf-8") as f:
        f.write(header + lines + ";\n\n#endif /* AI_AGENT_CHAT_PAGE_H */\n")

    # 自校验：把写出的内容解码回字节，必须与源文件完全一致
    text = open(DST, encoding="utf-8").read()
    body = text.split("=", 1)[1]
    out = bytearray()
    for piece in re.findall(r'"((?:[^"\\]|\\.)*)"', body):
        i = 0
        while i < len(piece):
            c = piece[i]
            if c != "\\":
                out.extend(c.encode("utf-8"))
                i += 1
                continue
            nxt = piece[i + 1]
            simple = {"n": 10, "r": 13, "t": 9, '"': 0x22, "\\": 0x5C}
            if nxt in simple:
                out.append(simple[nxt])
                i += 2
            elif nxt.isdigit():
                out.append(int(piece[i + 1:i + 4], 8))
                i += 4
            else:
                raise SystemExit("未知转义: \\%s" % nxt)

    if bytes(out) != data:
        for k in range(min(len(out), len(data))):
            if out[k] != data[k]:
                raise SystemExit(
                    "自校验失败：第 %d 字节不同（源 %02x，生成 %02x）"
                    % (k, data[k], out[k]))
        raise SystemExit("自校验失败：长度不同（源 %d，生成 %d）"
                         % (len(data), len(out)))

    print("chat_page.h 生成完成：%d 字节、%d 个片段，自校验通过"
          % (len(data), len(groups)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
