#!/usr/bin/env python3
"""Check the design-to-Qt handoff; never run a plugin or certify product tests.

Usage: python tools/check_qml_contract.py
Only Python's standard library is required. Planned files must stay explicitly
planned until an implementation change updates this handoff and its checker.
"""
from pathlib import Path
from html.parser import HTMLParser
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs" / "qml"


class ContractError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ContractError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)


def local_path(value):
    require(isinstance(value, str) and value and "\\" not in value,
            f"Expected repository-relative path: {value!r}")
    path = (ROOT / value).resolve()
    require(not Path(value).is_absolute() and path.is_relative_to(ROOT),
            f"Path outside repository: {value}")
    return path


def ids(items, label):
    require(isinstance(items, list), f"{label}: expected list")
    result = {item["id"] for item in items}
    require(len(result) == len(items), f"Duplicate {label} ID")
    return result


def refs(values, allowed, label, allow_empty=False):
    require(isinstance(values, list) and all(isinstance(x, str) for x in values),
            f"{label}: expected string list")
    require(allow_empty or values, f"{label}: missing references")
    require(len(set(values)) == len(values), f"{label}: duplicate references")
    require(set(values) <= allowed, f"{label}: unknown references {set(values) - allowed}")


def check_traceability(manifest):
    require(manifest["schemaVersion"] == 1, "Unsupported handoff schema")
    require(manifest["scope"] in ("DESIGN_ONLY", "QML_IMPLEMENTATION"), "Unknown scope")
    implementing = manifest["scope"] == "QML_IMPLEMENTATION"
    require(manifest["productImplementation"] == ("IN_PROGRESS" if implementing else "NOT_STARTED"), "Implementation status mismatch")
    require(manifest["productAcceptance"] in ("NOT_RUN", "PARTIAL"), "Static checks cannot certify full product acceptance")
    if implementing:
        require(local_path(manifest["implementationReport"]).is_file(), "Missing implementation evidence report")
    standard = local_path(manifest["standard"]).read_text(encoding="utf-8")
    features = (DOCS / "FUNCTIONAL_SPEC.md").read_text(encoding="utf-8")
    test_spec = (DOCS / "TEST_SPEC.md").read_text(encoding="utf-8")
    defined_features = re.findall(r"^### (F\d{2}) —", features, re.M)
    require(len(defined_features) == len(set(defined_features)), "Duplicate functional requirement")
    product_rows = list(re.finditer(r"^\| (T\d{2}) \| ([^|]+)\|", test_spec, re.M))
    test_map = {m[1]: set(re.findall(r"F\d{2}", m[2])) for m in product_rows}
    require(len(product_rows) == len(test_map), "Duplicate product test ID")
    require(all(fs and fs <= set(defined_features) for fs in test_map.values()), "Unmapped test requirement")
    prototype_ids = set(re.findall(r"^\| (R\d{2}) \|", test_spec, re.M))
    implemented_r = re.findall(r'run\("(R\d{2})",', (ROOT / "tools/check_qml_prototype.cjs").read_text(encoding="utf-8"))
    require(len(implemented_r) == len(set(implemented_r)) and set(implemented_r) == prototype_ids,
            "Prototype test IDs differ from TEST_SPEC")
    feature_ids = ids(manifest["requirements"], "requirement")
    require(feature_ids == set(defined_features), "Requirements differ from FUNCTIONAL_SPEC")
    page_ids = ids(manifest["pages"], "page")
    index_text = (DOCS / "README.md").read_text(encoding="utf-8")
    require(page_ids == set(re.findall(r"^\| (P\d{2}) \|", index_text, re.M)), "Pages differ from README")
    command_ids = ids(manifest["commands"], "command")
    require(command_ids == set(re.findall(r"^\| (C\d{2}) \|", standard, re.M)), "Commands differ from standard")
    component_ids = set(manifest["components"])
    for cid, component in manifest["components"].items():
        require(cid in standard, f"Component {cid} missing from standard")
        require(component["state"] in ("ADAPT", "PLANNED", "IMPLEMENTED"), f"Invalid component state: {cid}")
        require(implementing or component["state"] != "IMPLEMENTED", "Design-only cannot contain implemented components")
        require(component["paths"], f"Missing component paths: {cid}")
        for value in component["paths"]:
            path = local_path(value)
            require(path.suffix in (".h", ".cpp", ".qml"), f"Unexpected source path: {value}")
            require(path.is_file() if component["state"] != "PLANNED" else not path.exists(),
                    f"{cid}: {value} does not match {component['state']}; update implementation status explicitly")
    for page in manifest["pages"]:
        refs(page["components"], component_ids, page["id"])
        path = local_path(page["svg"])
        element = ET.parse(path).getroot()
        title = element.find("{http://www.w3.org/2000/svg}title")
        require(title is not None and (title.text or "").startswith(page["id"] + " ·"),
                f"Wrong SVG title for {page['id']}")
    for command in manifest["commands"]:
        refs(command["components"], component_ids, command["id"])
        refs(command["tests"], set(test_map), command["id"])
        require(command["entryPoint"], f"Missing entry point: {command['id']}")
    used_pages, used_commands, used_components, used_prototype = set(), set(), set(), set()
    for feature in manifest["requirements"]:
        fid = feature["id"]
        require(feature["acceptance"] in ("NOT_RUN", "PARTIAL"), f"{fid}: static check cannot mark product PASS")
        if feature["acceptance"] == "PARTIAL":
            require(implementing and feature.get("evidence"), f"{fid}: missing implementation evidence")
            for evidence in feature["evidence"]:
                source = local_path(evidence["source"]).read_text(encoding="utf-8")
                require(evidence["test"] in source, f"{fid}: evidence test absent from source")
        refs(feature["pages"], page_ids, fid)
        refs(feature["commands"], command_ids, fid)
        refs(feature["components"], component_ids, fid)
        refs(feature["tests"], set(test_map), fid)
        refs(feature["prototypeChecks"], prototype_ids, fid, allow_empty=True)
        expected = {tid for tid, fs in test_map.items() if fid in fs}
        require(set(feature["tests"]) == expected, f"{fid}: T coverage differs from TEST_SPEC: {expected}")
        used_pages.update(feature["pages"])
        used_commands.update(feature["commands"])
        used_components.update(feature["components"])
        used_prototype.update(feature["prototypeChecks"])
    require(used_pages == page_ids, "Orphaned page")
    require(used_commands == command_ids, "Orphaned command")
    require(used_components == component_ids, "Orphaned component")
    require(used_prototype == prototype_ids, "Unmapped prototype check")
    for value in manifest["plannedTestSources"]:
        require(not local_path(value).exists(), f"Test source now exists; update its status: {value}")
    for value in manifest.get("testSources", []):
        require(local_path(value).is_file(), f"Missing test source: {value}")
    return {"requirements": len(feature_ids), "pages": len(page_ids), "commands": len(command_ids),
            "components": len(component_ids), "product_test_groups": len(test_map),
            "prototype_test_groups": len(prototype_ids)}


def luminance(color):
    rgb = [int(color[i:i+2], 16) / 255 for i in (1, 3, 5)]
    rgb = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in rgb]
    return sum(x * weight for x, weight in zip(rgb, (0.2126, 0.7152, 0.0722)))


def check_tokens(tokens, manifest):
    require(tokens["schemaVersion"] == 1 and tokens["revision"] == manifest["designRevision"],
            "Token revision/schema differs from handoff")
    require(set(tokens["colors"]) == {"light", "dark"}, "Missing theme")
    keys = {"bg", "panel", "field", "ink", "muted", "line", "control", "accent", "onAccent",
            "accentHover", "soft", "green", "greenbg", "amber", "amberbg", "red", "redbg"}
    text_pairs = [(fg, bg) for fg in ("ink", "muted") for bg in ("bg", "panel", "field")]
    text_pairs += [("green", "greenbg"), ("amber", "amberbg"), ("red", "redbg"),
                   ("onAccent", "accent"), ("onAccent", "accentHover")]
    control_pairs = [(fg, bg) for fg in ("accent", "control") for bg in ("bg", "panel", "field")]
    checked = 0
    for theme, colors in tokens["colors"].items():
        require(set(colors) == keys, f"{theme}: incomplete semantic colors")
        require(all(re.fullmatch(r"#[0-9a-f]{6}", value) for value in colors.values()), "Invalid color")
        for pairs, minimum in ((text_pairs, 4.5), (control_pairs, 3.0)):
            for fg, bg in pairs:
                a, b = sorted((luminance(colors[fg]), luminance(colors[bg])))
                ratio = (b + 0.05) / (a + 0.05)
                require(ratio >= minimum, f"{theme} {fg}/{bg}: contrast {ratio:.2f} < {minimum}")
                checked += 1
    metrics = tokens["metrics"]
    require(all(isinstance(v, int) and v > 0 for v in metrics.values()), "Invalid metric")
    required = {"windowWidth": 1120, "windowHeight": 760, "minimumWidth": 900, "minimumHeight": 640,
                "pageMargin": 12, "spacingUnit": 6, "controlMinHeight": 26, "rowHeight": 28, "focusWidth": 2}
    require(all(metrics.get(k) == v for k, v in required.items()), "Metrics differ from approved layout")
    css = (DOCS / "prototype/prototype.css").read_text(encoding="utf-8")
    generated = (DOCS / "prototype/tokens.css").read_text(encoding="utf-8")
    definitions = set(re.findall(r"(--[\w-]+)\s*:", generated))
    require(not definitions.intersection(re.findall(r"(--[\w-]+)\s*:", css)), "Prototype overrides generated color tokens")
    available = definitions | set(re.findall(r"(--[\w-]+)\s*:", css))
    require(set(re.findall(r"var\((--[\w-]+)\)", css)) <= available, "Unknown CSS token")
    return checked


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.references, self.identifiers = [], []

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        self.references.extend(values[k] for k in ("src", "href") if k in values)
        if "id" in values:
            self.identifiers.append(values["id"])


def check_links():
    count = 0
    for path in DOCS.glob("*.md"):
        source = re.sub(r"```.*?```", "", path.read_text(encoding="utf-8"), flags=re.S)
        for value in re.findall(r"\[[^\]]+\]\(([^)]+)\)", source):
            require("://" not in value, f"Use local handoff references: {path.name}: {value}")
            target = (path.parent / value.split("#", 1)[0]).resolve()
            require(target.is_relative_to(ROOT) and target.is_file(), f"Broken link: {path.name}: {value}")
            count += 1
    for path in DOCS.rglob("*.html"):
        page = Page()
        page.feed(path.read_text(encoding="utf-8"))
        require(len(page.identifiers) == len(set(page.identifiers)), f"Duplicate HTML id: {path}")
        for value in page.references:
            if value.startswith("#"):
                require(value[1:] in page.identifiers, f"Missing anchor: {value}")
            else:
                target = (path.parent / value).resolve()
                require(target.is_relative_to(ROOT) and target.is_file(), f"Missing HTML asset: {value}")
            count += 1
    return count


def main():
    manifest = read_json(DOCS / "qt-traceability.json")
    counts = check_traceability(manifest)
    counts["token_contrast_pairs"] = check_tokens(read_json(local_path(manifest["tokens"])), manifest)
    counts["local_links"] = check_links()
    result = subprocess.run([sys.executable, str(ROOT / "tools/gen_qml_design.py"), "--check"],
                            cwd=ROOT, capture_output=True, text=True)
    require(result.returncode == 0, result.stdout + result.stderr)
    print(json.dumps({"status": "PASS", "scope": "DESIGN_CONTRACT_ONLY", **counts}, ensure_ascii=False))
    print(result.stdout.strip())
    print(f"Product implementation: {manifest['productImplementation']}. Product acceptance: {manifest['productAcceptance']} (not certified by this checker).")


if __name__ == "__main__":
    try:
        main()
    except (ContractError, OSError, KeyError, TypeError, ValueError, ET.ParseError) as error:
        print(f"Design contract check FAILED: {error}", file=sys.stderr)
        raise SystemExit(1)
