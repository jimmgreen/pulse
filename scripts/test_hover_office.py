"""Exercise sequential Office hover uploads in the real renderer, with isolated data."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import zipfile


def fixtures(root):
    root.mkdir(parents=True, exist_ok=True)
    paths = []
    for index in range(8):
        word = index % 2 == 0
        suffix = "docx" if word else "xlsx"
        path = root / f"sample-{index}.{suffix}"
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("[Content_Types].xml", '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="xml" ContentType="application/xml"/></Types>')
            if word:
                archive.writestr("word/document.xml", f'<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>Hover document {index}</w:t></w:r></w:p><w:p><w:r><w:t>Preview must finish while the pointer stays still.</w:t></w:r></w:p><w:sectPr><w:pgSz w:w="11906" w:h="16838"/></w:sectPr></w:body></w:document>')
            else:
                archive.writestr("xl/workbook.xml", '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Sheet1" sheetId="1" r:id="rId1"/></sheets></workbook>')
                archive.writestr("xl/_rels/workbook.xml.rels", '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/></Relationships>')
                archive.writestr("xl/worksheets/sheet1.xml", f'<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData><row r="1"><c r="A1" t="inlineStr"><is><t>Hover spreadsheet {index}</t></is></c><c r="B1"><v>42</v></c></row></sheetData></worksheet>')
        paths.append(str(path))
    return paths


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--prefetch", action="store_true")
    args = parser.parse_args()
    root = args.out.resolve()
    paths = fixtures(root / "fixtures")
    listing = root / "files.txt"
    listing.write_text("\n".join(paths), encoding="utf-8")
    report = root / "results.tsv"
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("PULSE_BENCH_HOVER_"):
            del env[key]
    env.update(PULSE_TEST_DATA_DIR=str(root / "profile"),
               PULSE_BENCH_HOVER_LIST=str(listing), PULSE_BENCH_HOVER_OUT=str(report),
               PULSE_BENCH_HOVER_TIMEOUT="2000", PULSE_TEST_PREVIEW_TRACE=str(root / "trace.log"))
    if args.prefetch:
        env.update(PULSE_BENCH_HOVER_PREFETCH="200", PULSE_BENCH_HOVER_PREFETCH_MODE="6")
    command = [str(args.exe.resolve()), "--test-instance", "--shot", str(root / "hover.png"),
               "--shot-hover-preview", "1" if args.prefetch else "0", str(root / "fixtures")]
    print("COMMAND " + subprocess.list2cmdline(command), flush=True)
    result = subprocess.run(command, env=env, timeout=120, capture_output=True)
    (root / "process.log").write_bytes(result.stdout + result.stderr)
    print(f"PROCESS exit={result.returncode}")
    if result.returncode or not report.exists():
        return 1
    rows = [line.split("\t") for line in report.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith(("#", "path\t"))]
    ok = len(rows) == len(paths)
    for row in rows:
        # RunHoverBench's data rows omit the legacy header's ext field.
        passed = row[2] == "bitmap" and int(row[3]) > 0 and int(row[4]) > 0 and float(row[8]) < 2000
        print(f"[{'PASS' if passed else 'FAIL'}] {Path(row[0]).name}: result={row[2]} size={row[3]}x{row[4]} cold_ms={row[5]} warm_ms={row[8]}")
        ok &= passed
    decoded = set()
    for trace in root.glob("trace.log.*.log"):
        for line in trace.read_text(encoding="utf-8", errors="replace").splitlines():
            if "stage=stored" in line and "ok=1 status=0" in line and "kind=0" in line:
                match = re.search(r"sample-\d+\.(?:docx|xlsx)", line)
                if match:
                    decoded.add(match[0])
    print(f"DECODE completed={len(decoded)}/{len(paths)}; preview host returned success")
    ok &= len(decoded) == len(paths)
    print(f"RESULT {'PASS' if ok else 'FAIL'} files={len(rows)}; no pointer input; isolated profile")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
