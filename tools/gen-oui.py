#!/usr/bin/env python3
"""
从 IEEE oui.csv 生成 main/oui_table.h（只保留常用厂商）。

用法：
    curl -sL https://standards-oui.ieee.org/oui/oui.csv -o oui.csv
    python3 tools/gen-oui.py oui.csv main/oui_table.h

想加厂商就往 PATTERNS 里加子串（大小写不敏感），重跑即可。
"""
import csv
import sys

# 厂商名匹配（子串，大小写不敏感）。按需增删。
PATTERNS = [
    # --- 芯片 / 模组 ---
    "espressif", "tuya", "realtek", "mediatek", "qualcomm", "broadcom",
    "cypress", "nordic", "texas instrument", "silicon lab", "microchip",
    "stmicro", "st micro", "nxp", "renesas", "infineon", "murata",
    "azurewave", "usi", "hon hai", "foxconn", "liteon", "wistron",
    "compal", "quanta", "pegatron", "askey", "sercomm", "sagemcom",
    "arcadyan", "technicolor", "shenzhen rf-link", "espress",
    # --- IoT / 家电 ---
    "xiaomi", "aqara", "lumi", "yeelight", "huami", "anker", "eufy",
    "roborock", "ecovacs", "dreame", "narwal", "sonoff", "itead",
    "shelly", "allterco", "broadlink", "orvibo", "ezviz", "hikvision",
    "dahua", "imou", "uniview", "midea", "haier", "gree", "hisense",
    "tcl", "skyworth", "changhong", "konka", "robam", "viomi",
    # --- 手机 / 电脑 ---
    "apple", "samsung", "lg electronics", "google", "nest", "amazon",
    "ring", "roku", "sonos", "signify", "philips", "lifx", "belkin",
    "wemo", "wyze", "netatmo", "bosch", "honeywell", "ecobee",
    "intel", "lenovo", "dell", "hp inc", "hewlett",
    # --- 网络设备 ---
    "tp-link", "tenda", "mercury", "ruijie", "h3c", "huawei", "zte",
    "netgear", "d-link", "asustek", "asus", "linksys", "ubiquiti",
    "aruba", "ruckus", "cisco", "juniper", "mikrotik", "fortinet",
    "zyxel", "draytek", "sophos", "watchguard", "palo alto",
    "synology", "qnap", "western digital", "seagate", "buffalo",
    # --- 开发板 ---
    "raspberry pi", "arduino", "particle", "pycom", "seeed", "waveshare",
]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]

    pats = [p.lower() for p in PATTERNS]
    rows = []
    with open(src, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            oui = r.get("Assignment", "").strip()
            org = r.get("Organization Name", "").strip()
            if len(oui) != 6 or not org:
                continue
            low = org.lower()
            if any(p in low for p in pats):
                rows.append((int(oui, 16), org))

    rows.sort()
    # 去重（同一 OUI 只保留第一条）
    uniq = []
    last = -1
    for oui, org in rows:
        if oui != last:
            uniq.append((oui, org))
            last = oui
    rows = uniq

    # 名字去重
    names, name_off = [], {}
    blob = bytearray()
    for _, org in rows:
        if org not in name_off:
            name_off[org] = len(blob)
            blob += org.encode("utf-8") + b"\0"
            names.append(org)

    if len(blob) > 65535:
        sys.exit("names blob too big: %d" % len(blob))

    with open(dst, "w", encoding="utf-8") as f:
        f.write("/* 自动生成，勿手改。由 tools/gen-oui.py 从 IEEE oui.csv 生成。 */\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("static const uint32_t OUI_KEY[] = {\n")
        for i in range(0, len(rows), 8):
            f.write("  " + ",".join("0x%06X" % o for o, _ in rows[i:i + 8]) + ",\n")
        f.write("};\n\n")
        f.write("static const uint16_t OUI_OFF[] = {\n")
        for i in range(0, len(rows), 12):
            f.write("  " + ",".join(str(name_off[o]) for _, o in rows[i:i + 12]) + ",\n")
        f.write("};\n\n")
        f.write("static const uint16_t OUI_N = %d;\n\n" % len(rows))
        f.write("static const char OUI_NAMES[] =\n")
        s = blob.decode("utf-8", errors="replace")
        for i in range(0, len(s), 60):
            chunk = s[i:i + 60]
            esc = (chunk.replace("\\", "\\\\").replace('"', '\\"')
                        .replace("\n", "").replace("\x00", "\\000"))
            f.write('  "%s"\n' % esc)
        f.write(";\n")

    print("entries: %d  vendors: %d  names: %d bytes" % (len(rows), len(names), len(blob)))


if __name__ == "__main__":
    main()
