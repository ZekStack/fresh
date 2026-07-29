#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "examples/CustomStorage/CustomStorage.ino"
text = path.read_text(encoding="utf-8")

replacements = [
    ("#include <memory>\n", "#include <memory>\n#include <new>\n"),
    (
        "\t\tstd::copy_n(_bytes.data() + _position, count, buffer);\n\t\t_position += count;\n",
        "\t\tif (count == 0) return 0;\n\t\tstd::copy_n(_bytes.data() + _position, count, buffer);\n\t\t_position += count;\n",
    ),
    (
        "\t\tbackend.reset(new MemoryFileBackend(found->second, mode));\n",
        "\t\tbackend.reset(new (std::nothrow) MemoryFileBackend(found->second, mode));\n",
    ),
    (
        '''void require(bool condition, const char *message) {
\tif (condition) return;
\tSerial.print("FAILED: ");
\tSerial.println(message);
\twhile (true) delay(1000);
}
''',
        '''void require(bool condition, const char *message) {
\tif (condition) return;
\tSerial.print("FAILED: ");
\tSerial.println(message);
\twhile (true) delay(1000);
}

void require(const FreshResult &result, const char *message) {
\trequire(static_cast<bool>(result), message);
}

void require(const FreshModelResult &result, const char *message) {
\trequire(static_cast<bool>(result), message);
}
''',
    ),
]

for old, new in replacements:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, found {count}: {old!r}")
    text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
print("custom storage example fixed")
