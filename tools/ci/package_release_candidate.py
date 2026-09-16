#!/usr/bin/env python3

import argparse
import gzip
import hashlib
import io
import json
import os
import shutil
import tarfile
import tempfile
from pathlib import Path


def sha256(path: Path) -> str:
	h = hashlib.sha256()
	with path.open("rb") as handle:
		for chunk in iter(lambda: handle.read(1024 * 1024), b""):
			h.update(chunk)
	return h.hexdigest()


def add_file(tar: tarfile.TarFile, source: Path, arcname: str, mode: int) -> None:
	data = source.read_bytes()
	info = tarfile.TarInfo(arcname)
	info.size = len(data)
	info.mode = mode
	info.mtime = 0
	info.uid = 0
	info.gid = 0
	info.uname = ""
	info.gname = ""
	tar.addfile(info, io.BytesIO(data))


def main() -> int:
	parser = argparse.ArgumentParser(description="Create deterministic Sentum release-candidate bundle")
	parser.add_argument("--binary", default="sentum")
	parser.add_argument("--readiness", required=True)
	parser.add_argument("--git-sha", default=os.environ.get("GITHUB_SHA", "unknown"))
	parser.add_argument("--output-dir", default="artifacts")
	parser.add_argument("--report", default="log/rc_package.json")
	parser.add_argument("--summary", default="log/rc_package.md")
	args = parser.parse_args()

	binary = Path(args.binary)
	readiness_path = Path(args.readiness)
	if not binary.is_file():
		raise SystemExit(f"release binary missing: {binary}")
	if not readiness_path.is_file():
		raise SystemExit(f"release readiness evidence missing: {readiness_path}")

	readiness = json.loads(readiness_path.read_text(encoding="utf-8"))
	if readiness.get("status") != "PASS":
		raise SystemExit("release readiness evidence is not PASS")
	if readiness.get("git_sha") != args.git_sha:
		raise SystemExit("release readiness evidence does not match requested commit")

	root_name = f"sentum-rc-{args.git_sha[:12]}"
	output_dir = Path(args.output_dir)
	output_dir.mkdir(parents=True, exist_ok=True)
	report_path = Path(args.report)
	summary_path = Path(args.summary)
	report_path.parent.mkdir(parents=True, exist_ok=True)
	summary_path.parent.mkdir(parents=True, exist_ok=True)

	safe_config = sorted(Path("config").glob("*.example.json"))
	if Path("config/risk.json").is_file():
		safe_config.append(Path("config/risk.json"))

	doc_names = [
		"ACCOUNT_RECONCILIATION.md",
		"LIVE_TRADING_SAFETY.md",
		"OPERATIONAL_ACCEPTANCE.md",
		"OPERATIONAL_SAFETY_UX.md",
		"RELEASE_READINESS.md",
		"RUNTIME_LIFECYCLE.md",
	]

	package_inputs = [(binary, "bin/sentum", 0o755)]
	for path in safe_config:
		package_inputs.append((path, str(path), 0o644))
	for name in doc_names:
		path = Path("docs") / name
		if not path.is_file():
			raise SystemExit(f"required handoff document missing: {path}")
		package_inputs.append((path, str(path), 0o644))
	for name in ("README.md", "LICENSE.md", "DISCLAIMER.md"):
		path = Path(name)
		if not path.is_file():
			raise SystemExit(f"required release file missing: {path}")
		package_inputs.append((path, name, 0o644))
	package_inputs.append((readiness_path, "evidence/release_readiness.json", 0o644))

	manifest_files = []
	for source, destination, _ in package_inputs:
		manifest_files.append({
			"path": destination,
			"sha256": sha256(source),
			"size": source.stat().st_size,
		})

	manifest = {
		"schema_version": 1,
		"status": "PASS",
		"git_sha": args.git_sha,
		"bundle_root": root_name,
		"release_readiness_sha256": sha256(readiness_path),
		"files": manifest_files,
		"excluded": ["config/config.json", "runtime databases", "logs", "credentials", "secrets"],
	}
	manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")

	checksum_lines = [f"{entry['sha256']}  {entry['path']}" for entry in manifest_files]
	manifest_digest = hashlib.sha256(manifest_bytes).hexdigest()
	checksum_lines.append(f"{manifest_digest}  MANIFEST.json")
	checksums_bytes = ("\n".join(checksum_lines) + "\n").encode("utf-8")

	archive_path = output_dir / f"{root_name}.tar.gz"
	with archive_path.open("wb") as raw:
		with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as gz:
			with tarfile.open(fileobj=gz, mode="w") as tar:
				for source, destination, mode in sorted(package_inputs, key=lambda item: item[1]):
					add_file(tar, source, f"{root_name}/{destination}", mode)
				manifest_info = tarfile.TarInfo(f"{root_name}/MANIFEST.json")
				manifest_info.size = len(manifest_bytes)
				manifest_info.mode = 0o644
				manifest_info.mtime = 0
				manifest_info.uid = manifest_info.gid = 0
				manifest_info.uname = manifest_info.gname = ""
				tar.addfile(manifest_info, io.BytesIO(manifest_bytes))
				checksums_info = tarfile.TarInfo(f"{root_name}/SHA256SUMS")
				checksums_info.size = len(checksums_bytes)
				checksums_info.mode = 0o644
				checksums_info.mtime = 0
				checksums_info.uid = checksums_info.gid = 0
				checksums_info.uname = checksums_info.gname = ""
				tar.addfile(checksums_info, io.BytesIO(checksums_bytes))

	archive_digest = sha256(archive_path)
	checksum_path = archive_path.with_suffix(archive_path.suffix + ".sha256")
	checksum_path.write_text(f"{archive_digest}  {archive_path.name}\n", encoding="utf-8")

	report = {
		"schema_version": 1,
		"status": "PASS",
		"git_sha": args.git_sha,
		"archive": str(archive_path),
		"archive_sha256": archive_digest,
		"archive_size": archive_path.stat().st_size,
		"manifest": manifest,
	}
	report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
	summary_path.write_text(
		"# Sentum release candidate package\n\n"
		f"**Status:** PASS\n\n"
		f"Commit: `{args.git_sha}`\n\n"
		f"Archive: `{archive_path.name}`\n\n"
		f"SHA-256: `{archive_digest}`\n\n"
		"The bundle excludes local runtime configuration, databases, logs, credentials and secrets.\n",
		encoding="utf-8",
	)
	print(summary_path.read_text(encoding="utf-8"), end="")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
