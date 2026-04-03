import re
from argparse import ArgumentParser, Namespace
from enum import Enum, auto
from pathlib import Path


class Pass(Enum):
    COUNT_ENUMS = auto()
    WRITE_OUTPUT = auto()


parser: ArgumentParser = ArgumentParser()
parser.add_argument("in_file", type=Path)
parser.add_argument("out_file", type=Path)
args: Namespace = parser.parse_args()

print(f"Sanitizing enums in {args.in_file.name}")

args.out_file.parent.mkdir(exist_ok=True, parents=True)

# Keeps tracks of the number of times each enum has been encountered per pass
enum_counters: dict[Pass, dict[str, int]] = {}

namespace: str = ""
in_enum: bool = False
with open(args.in_file, "rt") as in_file, open(args.out_file, "wt") as out_file:
    out_file.write("#ifndef SDK_ENUMS_INTERNAL_HPP_\n")
    out_file.write("#define SDK_ENUMS_INTERNAL_HPP_\n")

    for p in Pass:
        print(f"Pass {p.value}/{len(Pass)}: {p.name}")
        enum_counters[p] = {}

        # Parse every line in the input file
        in_file.seek(0)
        for line in in_file:
            line = line.rstrip()

            # Match start of namespace
            if m := re.match(
                r"^\s*namespace\s(?P<ns_name>[a-zA-Z_]\w*(::[a-zA-Z_]\w*)*)\s*{", line
            ):
                namespace = m["ns_name"]
            # Match end of enum or namespace
            if m := re.match(r"}", line):
                if not in_enum:
                    namespace = ""
                in_enum = False

            # Match start of named enum
            if m := re.match(
                r"^(?P<indent>\s*)enum\b(\s*(class|struct)\b)?\s*(?P<enum_head_name>[a-zA-Z_]\w*)(?P<enum_base>:\s*([a-zA-Z_]\w*))?\s*{",
                line,
            ):
                enum_name: str = m["enum_head_name"]
                enum_name_full: str = (
                    namespace + "::" + enum_name if namespace else enum_name
                )

                # Count number of occurrences of enum
                if enum_name_full not in enum_counters[p]:
                    enum_counters[p][enum_name_full] = 0
                counter: int = enum_counters[p][enum_name_full]
                counter = counter + 1
                enum_counters[p][enum_name_full] = counter

                if p == Pass.WRITE_OUTPUT:
                    # Deduplicate enum name
                    if enum_counters[Pass.COUNT_ENUMS][enum_name_full] > 1:
                        enum_name = enum_name + "__" + str(counter)

                    # Rewrite enum to enum class
                    line = (
                        f"{m["indent"]}enum class {enum_name}{m["enum_base"] or ""} {{"
                    )

            if p == Pass.WRITE_OUTPUT:
                out_file.write(line + "\n")

    out_file.write("#endif\n")
