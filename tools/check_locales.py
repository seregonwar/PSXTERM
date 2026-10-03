"""Validate client locale keys, indexed placeholders, and resource types."""

import json
from pathlib import Path
import re
import sys


def read_locale(path):
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate key: {key}")
            if not isinstance(value, str):
                raise ValueError(f"{key}: translation must be a string")
            result[key] = value
        return result

    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_pairs)


def validate(directory):
    reference = read_locale(directory / "en.json")
    errors = []
    for path in sorted(directory.glob("*.json")):
        translated = read_locale(path)
        for key in sorted(reference.keys() - translated.keys()):
            errors.append(f"{path.name}: missing {key}")
        for key in sorted(translated.keys() - reference.keys()):
            errors.append(f"{path.name}: unexpected {key}")
        for key in reference.keys() & translated.keys():
            expected = sorted(re.findall(r"\{\d+\}", reference[key]))
            actual = sorted(re.findall(r"\{\d+\}", translated[key]))
            if expected != actual:
                errors.append(f"{path.name}: placeholders differ for {key}")
            if reference[key].strip() and not translated[key].strip():
                errors.append(f"{path.name}: empty translation for {key}")
    return errors


if __name__ == "__main__":
    directory = Path(__file__).resolve().parents[1] / "client/tui/locales"
    try:
        problems = validate(directory)
    except (OSError, ValueError) as error:
        problems = [str(error)]
    if problems:
        print("\n".join(problems), file=sys.stderr)
        sys.exit(1)
    print("Client locales: keys, values, and placeholders match.")
