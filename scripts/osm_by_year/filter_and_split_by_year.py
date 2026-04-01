#!/usr/bin/env python3
"""
Filter OSM CSV by year range (2014-2024), remove duplicates by ID, 
and split into yearly files.

Input: CSV with id,ts columns
Output: 10 yearly files (2014-2024) with format:
  - First line: <num_records> 2
  - Following lines: <id> <year>
"""

import sys
import csv
from datetime import datetime
from collections import defaultdict


def process_and_split(input_file, output_dir, output_prefix):
    """
    Filter by year (2014-2024), deduplicate by ID, split into yearly files.
    
    Args:
        input_file: CSV file with id,ts columns
        output_dir: Directory for output files
        output_prefix: Prefix for output files
    """
    import os
    
    print(f"Processing {input_file}...")
    print(f"Filtering years 2014-2024, removing duplicates by ID...\n")
    
    # Track unique IDs globally to remove duplicates
    seen_ids_global = set()
    year_data = defaultdict(list)
    
    total_processed = 0
    total_filtered = 0
    total_duplicates = 0
    year_duplicates = defaultdict(int)
    
    with open(input_file, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        
        for row in reader:
            total_processed += 1
            
            # Parse year from timestamp
            try:
                timestamp_str = row['ts'].split('+')[0].split('.')[0]
                timestamp = datetime.strptime(timestamp_str, "%Y-%m-%d %H:%M:%S")
                year = timestamp.year
            except Exception as e:
                print(f"Warning: Could not parse timestamp '{row['ts']}': {e}")
                continue
            
            # Filter by year range
            if year < 2014 or year > 2024:
                total_filtered += 1
                continue
            
            record_id = row['id']
            
            # Check for duplicate ID
            if record_id in seen_ids_global:
                total_duplicates += 1
                year_duplicates[year] += 1
                continue
            
            # Mark as seen and store
            seen_ids_global.add(record_id)
            year_data[year].append(record_id)
            
            # Progress update
            if total_processed % 1000000 == 0:
                kept = sum(len(records) for records in year_data.values())
                print(f"  Processed {total_processed:,} rows, kept {kept:,} unique records...")
    
    # Write output files
    print(f"\nWriting output files to {output_dir}...")
    os.makedirs(output_dir, exist_ok=True)
    
    for year in range(2014, 2025):  # 2014 to 2024 inclusive
        filename = os.path.join(output_dir, f"{output_prefix}_{year}.csv")
        records = year_data.get(year, [])
        num_records = len(records)
        
        with open(filename, 'w', encoding='utf-8') as f:
            # First line: <num_records> 2
            f.write(f"{num_records} 2\n")
            
            # Following lines: <id> <year>
            for record_id in records:
                f.write(f"{record_id} {year}\n")
        
        print(f"  Created: {filename} with {num_records:,} records")
    
    # Summary
    print(f"\nProcessing complete!")
    print(f"Total records processed: {total_processed:,}")
    print(f"  Filtered out (not 2014-2024): {total_filtered:,}")
    print(f"  Duplicates removed: {total_duplicates:,}")
    print(f"  Unique records written: {sum(len(records) for records in year_data.values()):,}")
    print()
    
    print("Records by year:")
    for year in range(2014, 2025):
        num_records = len(year_data.get(year, []))
        dups = year_duplicates.get(year, 0)
        print(f"  {year}: {num_records:,} unique records ({dups:,} duplicates removed)")


def main():
    if len(sys.argv) < 2:
        print("Usage: python filter_and_split_by_year.py <input.csv> [output_dir] [output_prefix]")
        print("\nArguments:")
        print("  input.csv     - CSV file with id,ts columns")
        print("  output_dir    - Directory for output files (default: current directory)")
        print("  output_prefix - Prefix for output files (default: osm_output)")
        print("\nOutput: 10 files named {prefix}_2014.csv to {prefix}_2024.csv")
        print("Format per file:")
        print("  Line 1: <num_records> 2")
        print("  Line 2+: <id> <year>")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) >= 3 else '.'
    output_prefix = sys.argv[3] if len(sys.argv) >= 4 else 'osm_output'
    
    process_and_split(input_file, output_dir, output_prefix)


if __name__ == '__main__':
    main()
