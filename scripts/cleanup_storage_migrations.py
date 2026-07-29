#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FILES = [
    ".github/workflows/add-result-aware-storage-info.yml",
    ".github/workflows/add-storage-source-audit.yml",
    ".github/workflows/apply-custom-storage-init.yml",
    ".github/workflows/apply-storage-context-migration.yml",
    ".github/workflows/apply-storage-hardening.yml",
    ".github/workflows/cleanup-storage-symbols.yml",
    ".github/workflows/extend-custom-storage-conformance.yml",
    ".github/workflows/fix-custom-storage-example.yml",
    ".github/workflows/generalize-persistence-streams.yml",
    ".github/workflows/harden-storage-durability.yml",
    ".github/workflows/protect-database-storage-root.yml",
    ".github/workflows/storage-validation.yml",
    ".github/workflows/update-storage-documentation.yml",
    ".github/workflows/cleanup-storage-migrations.yml",
    "scripts/add_result_aware_storage_info.py",
    "scripts/add_storage_source_audit.py",
    "scripts/apply_custom_storage_init.py",
    "scripts/apply_storage_context_migration.py",
    "scripts/apply_storage_hardening.py",
    "scripts/cleanup_storage_symbols.py",
    "scripts/extend_custom_storage_conformance.py",
    "scripts/fix_custom_storage_example.py",
    "scripts/generalize_persistence_streams.py",
    "scripts/harden_storage_durability.py",
    "scripts/protect_database_storage_root.py",
    "scripts/update_storage_documentation.py",
    "scripts/cleanup_storage_migrations.py",
]

removed = []
for relative in FILES:
    path = ROOT / relative
    if path.exists():
        path.unlink()
        removed.append(relative)

print(f"removed {len(removed)} migration-only files")
for relative in removed:
    print(relative)
