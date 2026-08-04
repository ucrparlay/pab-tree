#!/usr/bin/env python3
"""
Generates .clangd configuration from compile_commands.json
Run this after cmake to update clangd settings with include paths and C++ standard.
"""

import json
import re
from pathlib import Path


def extract_includes_and_std(compile_commands_path):
    """Extract unique include paths and C++ standard from compile_commands.json"""
    with open(compile_commands_path, 'r') as f:
        commands = json.load(f)

    includes = set()
    std_flag = '-std=gnu++20'  # Default

    for entry in commands:
        command = entry.get('command', '')

        # Extract -I flags
        for match in re.finditer(r'-I(\S+)', command):
            path = match.group(1)
            # Filter out system paths and googletest
            if not path.startswith('/usr/') and 'googletest' not in path:
                includes.add(path)

        # Extract -isystem flags
        for match in re.finditer(r'-isystem\s+(\S+)', command):
            path = match.group(1)
            if not path.startswith('/usr/') and 'googletest' not in path:
                includes.add(path)

        # Extract C++ standard (use first found)
        std_match = re.search(r'-std=(gnu\+\+\d+|c\+\+\d+)', command)
        if std_match:
            std_flag = f'-std={std_match.group(1)}'

    return sorted(includes), std_flag


def generate_clangd_config(includes, std_flag, output_path):
    """Generate .clangd configuration file"""
    config = "CompileFlags:\n  Add:\n"
    config += f"    - {std_flag}\n"

    for include in includes:
        config += f"    - -I{include}\n"

    with open(output_path, 'w') as f:
        f.write(config)


def main():
    # Paths
    repo_root = Path(__file__).parent.parent
    compile_commands = repo_root / 'build' / 'compile_commands.json'
    clangd_config = repo_root / '.clangd'

    if not compile_commands.exists():
        print(f"WARNING: {compile_commands} not found. Run cmake first.")
        return 1

    print(f"Reading {compile_commands}...")
    includes, std_flag = extract_includes_and_std(compile_commands)

    print(f"\nGenerating {clangd_config}...")
    print(f"  C++ Standard: {std_flag}")
    print(f"  Include paths: {len(includes)}")

    generate_clangd_config(includes, std_flag, clangd_config)
    print(f"\n✓ Generated {clangd_config}")

    return 0


if __name__ == '__main__':
    exit(main())
