#!/usr/bin/env python3
"""
Filter OSM CSV file by year range (2014-2024).
Remove duplicate records with the same Morton code (keep first occurrence).
Split data into separate files by year.
"""

import sys
import csv
from datetime import datetime
from collections import defaultdict


def filter_and_split_by_year(input_file, output_prefix):
    """
    Filter records between 2014-2024, remove Morton duplicates globally, and split by year.
    
    IMPORTANT: Morton codes are unique across ALL years. If a Morton code appears in multiple
    years, only the first occurrence (earliest year) is kept.
    
    Output format for each year file:
    - First line: <num_records> 2
    - Following lines: <morton_code> <id>
    
    Args:
        input_file: Input CSV file with id, ts, lon, lat, morton columns
        output_prefix: Prefix for output files (will create {prefix}_2014.csv, etc.)
    """
    print(f"Processing {input_file}...")
    print("Filtering: Year 2014-2024")
    print("Removing duplicates by Morton code GLOBALLY (keeping first occurrence across all years)\n")
    
    # Track seen Morton codes GLOBALLY across all years
    seen_morton_global = set()
    
    # Store records for each year (will write at the end to know count)
    year_data = defaultdict(list)
    duplicate_counts = defaultdict(int)
    global_duplicates = 0
    
    total_processed = 0
    total_filtered = 0
    
    with open(input_file, 'r', encoding='utf-8') as infile:
        reader = csv.DictReader(infile)
        
        # Verify required columns
        if not all(col in reader.fieldnames for col in ['id', 'ts', 'lon', 'lat', 'morton']):
            print("ERROR: Input file must have columns: id, ts, lon, lat, morton")
            sys.exit(1)
        
        for row in reader:
            total_processed += 1
            
            # Parse timestamp to extract year
            try:
                # Handle format like "2018-10-09 17:10:01+00:00"
                timestamp_str = row['ts'].split('+')[0].split('.')[0]  # Remove timezone and microseconds
                timestamp = datetime.strptime(timestamp_str, "%Y-%m-%d %H:%M:%S")
                year = timestamp.year
            except Exception as e:
                print(f"Warning: Could not parse timestamp '{row['ts']}': {e}")
                continue
            
            # Filter by year range
            if year < 2014 or year > 2024:
                continue
            
            total_filtered += 1
            
            # Check for duplicate Morton code GLOBALLY
            morton_code = row['morton']
            if morton_code in seen_morton_global:
                duplicate_counts[year] += 1
                global_duplicates += 1
                continue
            
            # Mark this Morton code as seen GLOBALLY
            seen_morton_global.add(morton_code)
            
            # Store record data (morton, id)
            year_data[year].append((morton_code, row['id']))
            
            # Progress update
            if total_processed % 1000000 == 0:
                total_kept = sum(len(records) for records in year_data.values())
                print(f"  Processed {total_processed:,} rows, kept {total_kept:,} unique records...")
        
        # Now write all files with correct format
        print(f"\nWriting output files...")
        for year in sorted(year_data.keys()):
            filename = f"{output_prefix}_{year}.csv"
            records = year_data[year]
            num_records = len(records)
            
            with open(filename, 'w', encoding='utf-8') as f:
                # First line: <num_records> 2
                f.write(f"{num_records} 2\n")
                
                # Following lines: <morton_code> <id>
                for morton, record_id in records:
                    f.write(f"{morton} {record_id}\n")
            
            print(f"Created: {filename} with {num_records:,} records")
        
        print(f"\nProcessing complete!")
        print(f"Total rows processed: {total_processed:,}")
        print(f"Rows in year range (2014-2024): {total_filtered:,}")
        print(f"Total duplicates removed (globally): {global_duplicates:,}")
        print(f"Total unique Morton codes written: {sum(len(records) for records in year_data.values()):,}")
        print(f"Unique Morton codes globally: {len(seen_morton_global):,}")
        print(f"\n{'='*60}")
        print(f"TOTAL FILTERED RECORDS (2014-2024): {total_filtered:,}")
        print(f"{'='*60}\n")
        
        print("Records by year:")
        for year in sorted(year_data.keys()):
            dups = duplicate_counts[year]
            num_records = len(year_data[year])
            print(f"  {year}: {num_records:,} unique records ({dups:,} duplicates removed)")


def main():
    if len(sys.argv) < 2:
        print("Usage: python filter_by_year.py <input.csv> [output_prefix]")
        print("\nExample: python filter_by_year.py data.csv output")
        print("  Creates: output_2014.csv, output_2015.csv, ..., output_2024.csv")
        sys.exit(1)
    
    input_file = sys.argv[1]
    
    # Determine output prefix
    if len(sys.argv) >= 3:
        output_prefix = sys.argv[2]
    else:
        # Default: use input filename without extension
        output_prefix = input_file.replace('.csv', '')
    
    filter_and_split_by_year(input_file, output_prefix)


if __name__ == '__main__':
    main()
