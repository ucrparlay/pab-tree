#!/usr/bin/env python3
"""
Process filtered OSM CSV: shift coordinates, compute Morton codes, 
remove duplicates, and split into yearly files.

This is step 3: process only the filtered records (2014-2024).
"""

import sys
import csv
from datetime import datetime
from collections import defaultdict


def interleave_bits(x, y):
    """Compute Morton code by interleaving bits of x and y coordinates."""
    morton = 0
    for i in range(32):
        morton |= ((x & (1 << i)) << i) | ((y & (1 << i)) << (i + 1))
    return morton


def compute_morton_code(lon, lat, lon_scale=1e7, lat_scale=1e7):
    """Compute Morton code from longitude and latitude."""
    x = int(lon * lon_scale)
    y = int(lat * lat_scale)
    return interleave_bits(x, y)


def process_and_split(input_file, output_prefix, output_dir='.'):
    """
    Shift coordinates, compute Morton codes, remove duplicates, split by year.
    
    Args:
        input_file: Filtered CSV file with id, ts, lon, lat columns (2014-2024 only)
        output_prefix: Prefix for output files
        output_dir: Directory for output files (default: current directory)
    """
    print(f"Processing {input_file}...")
    print("This will: shift coordinates → compute Morton codes → remove duplicates → split by year\n")
    
    # First pass: find min coordinates
    print("Pass 1/2: Finding minimum coordinates...")
    min_lon = float('inf')
    min_lat = float('inf')
    total_records = 0
    
    with open(input_file, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            lon = float(row['lon'])
            lat = float(row['lat'])
            min_lon = min(min_lon, lon)
            min_lat = min(min_lat, lat)
            total_records += 1
            
            if total_records % 1000000 == 0:
                print(f"  Scanned {total_records:,} rows...")
    
    print(f"  Minimum lon: {min_lon:.7f}, lat: {min_lat:.7f}")
    print(f"  Total records: {total_records:,}\n")
    
    # Calculate shifts
    shift_lon = -min_lon if min_lon < 0 else 0
    shift_lat = -min_lat if min_lat < 0 else 0
    
    print(f"Pass 2/2: Processing records...")
    print(f"  Coordinate shifts: lon +{shift_lon:.7f}, lat +{shift_lat:.7f}")
    print(f"  Computing Morton codes and removing duplicates...\n")
    
    # Second pass: process records
    seen_morton_global = set()
    year_data = defaultdict(list)
    duplicate_counts = defaultdict(int)
    global_duplicates = 0
    processed = 0
    
    with open(input_file, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        
        for row in reader:
            processed += 1
            
            # Parse year
            try:
                timestamp_str = row['ts'].split('+')[0].split('.')[0]
                timestamp = datetime.strptime(timestamp_str, "%Y-%m-%d %H:%M:%S")
                year = timestamp.year
            except Exception as e:
                print(f"Warning: Could not parse timestamp '{row['ts']}': {e}")
                continue
            
            # Shift coordinates
            new_lon = float(row['lon']) + shift_lon
            new_lat = float(row['lat']) + shift_lat
            
            # Compute Morton code
            morton_code = compute_morton_code(new_lon, new_lat)
            
            # Check for global duplicate
            if morton_code in seen_morton_global:
                duplicate_counts[year] += 1
                global_duplicates += 1
                continue
            
            # Mark as seen and store
            seen_morton_global.add(morton_code)
            year_data[year].append((morton_code, row['id']))
            
            if processed % 1000000 == 0:
                kept = sum(len(records) for records in year_data.values())
                print(f"  Processed {processed:,} rows, kept {kept:,} unique records...")
    
    # Write output files
    import os
    print(f"\nWriting output files to {output_dir}...")
    for year in sorted(year_data.keys()):
        filename = os.path.join(output_dir, f"{output_prefix}_{year}.csv")
        records = year_data[year]
        num_records = len(records)
        
        with open(filename, 'w', encoding='utf-8') as f:
            # First line: <num_records> 2
            f.write(f"{num_records} 2\n")
            
            # Following lines: <morton_code> <id>
            for morton, record_id in records:
                f.write(f"{morton} {record_id}\n")
        
        print(f"  Created: {filename} with {num_records:,} records")
    
    print(f"\nProcessing complete!")
    print(f"Total records processed: {processed:,}")
    print(f"Total duplicates removed (globally): {global_duplicates:,}")
    print(f"Total unique Morton codes written: {sum(len(records) for records in year_data.values()):,}")
    print(f"Unique Morton codes globally: {len(seen_morton_global):,}\n")
    
    print("Records by year:")
    for year in sorted(year_data.keys()):
        dups = duplicate_counts[year]
        num_records = len(year_data[year])
        print(f"  {year}: {num_records:,} unique records ({dups:,} duplicates removed)")


def main():
    if len(sys.argv) < 2:
        print("Usage: python compute_morton_and_split.py <filtered.csv> [output_dir] [output_prefix]")
        print("\nArguments:")
        print("  filtered.csv  - Input CSV file (pre-filtered to year range 2014-2024)")
        print("  output_dir    - Directory for output files (optional, default: current directory)")
        print("  output_prefix - Prefix for output files (optional, default: input filename)")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) >= 3 else '.'
    output_prefix = sys.argv[3] if len(sys.argv) >= 4 else input_file.replace('.csv', '')
    
    process_and_split(input_file, output_prefix, output_dir)


if __name__ == '__main__':
    main()
