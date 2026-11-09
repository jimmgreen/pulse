"""Bounded, repository-owned template inputs shared by SDK generation and CLI."""
import json
from pathlib import Path

DIRECTORY = Path(__file__).resolve().parents[2] / "examples/native"


def read_json(path, limit):
    with path.open("rb") as stream:
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        raise ValueError("Template input exceeds its byte limit")

    def unique(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("Duplicate template JSON key: " + key)
            result[key] = value
        return result

    return json.loads(raw.decode("utf-8"), object_pairs_hook=unique,
        parse_constant=lambda value: (_ for _ in ()).throw(ValueError("Invalid template JSON constant: " + value)))


def names(directory=DIRECTORY):
    catalog = read_json(directory / "catalog.json", 4096)
    if not isinstance(catalog, dict) or set(catalog) != {"formatVersion", "templates"} or type(catalog["formatVersion"]) is not int or catalog["formatVersion"] != 1:
        raise ValueError("Unsupported template catalog")
    values = catalog["templates"]
    allowed = set("abcdefghijklmnopqrstuvwxyz0123456789-_")
    if not isinstance(values, list) or not 1 <= len(values) <= 32 or any(
        not isinstance(name, str) or not 1 <= len(name) <= 64 or not set(name) <= allowed for name in values):
        raise ValueError("Invalid template identifiers")
    if len(set(values)) != len(values):
        raise ValueError("Duplicate template identifiers")
    return tuple(values)


def load(name, directory=DIRECTORY):
    if name not in names(directory):
        raise ValueError("Unknown template")
    manifest = read_json(directory / name / "manifest.json", 65536)
    if not isinstance(manifest, dict):
        raise ValueError("Template manifest must be an object")
    return manifest


def all_templates(directory=DIRECTORY):
    return {name: load(name, directory) for name in names(directory)}
