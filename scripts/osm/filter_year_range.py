#!/usr/bin/env python3
"""
Filter OSM CSV file by year range (2014-2024).
This is step 2: filter BEFORE computing Morton codes to save processing time.
"""

import sys
import csv
from datetime import datetime


def filter_by_year_range(input_file, output_file, start_year=2014, end_year=2024):
    """
    Filter records to only keep those within the year range.
    
    Args:
        input_file: Input CSV file with id, ts, lon, lat columns
        output_file: Output CSV file with filtered records
        start_year: Start year (inclusive, default 2014)
        end_year: End year (inclusive, default 2024)
    """
    print(f"Filtering {input_file} for years {start_year}-{end_year}...")
    
    total_processed = 0
    total_kept = 0
    
    with open(input_file, 'r', encoding='utf-8') as infile, \
         open(output_file, 'w', newline='', encoding='utf-8') as outfile:
        
        reader = csv.DictReader(infile)
        writer = csv.DictWriter(outfile, fieldnames=['id', 'ts', 'lon', 'lat'])
        writer.writeheader()
        
        # Verify required columns
        if not all(col in reader.fieldnames for col in ['id', 'ts', 'lon', 'lat']):
            print("ERROR: Input file must have columns: id, ts, lon, lat")
            sys.exit(1)
        
        for row in reader:
            total_processed += 1
            
            # Parse timestamp to extract year
            try:
                # Handle format like "2018-10-09 17:10:01+00:00"
                timestamp_str = row['ts'].split('+')[0].split('.')[0]
                timestamp = datetime.strptime(timestamp_str, "%Y-%m-%d %H:%M:%S")
                year = timestamp.year
            except Exception as e:
                print(f"Warning: Could not parse timestamp '{row['ts']}': {e}")
                continue
            
            # Filter by year range
            if year < start_year or year > end_year:
                continue
            
            # Keep this record
            writer.writerow(row)
            total_kept += 1
            
            # Progress update
            if total_processed % 1000000 == 0:
                print(f"  Processed {total_processed:,} rows, kept {total_kept:,} records...")
    
    print(f"\nFiltering complete!")
    print(f"Total rows processed: {total_processed:,}")
    print(f"{'='*60}")
    print(f"TOTAL FILTERED RECORDS ({start_year}-{end_year}): {total_kept:,}")
    print(f"{'='*60}")
    print(f"Filtered records saved to: {output_file}\n")
    
    return total_kept


def main():
    if len(sys.argv) < 2:
        print("Usage: python filter_year_range.py <input.csv> [output.csv]")
        print("\nFilters records to year range 2014-2024")
        sys.exit(1)
    
    input_file = sys.argv[1]
    
    # Determine output file name
    if len(sys.argv) >= 3:
        output_file = sys.argv[2]
    else:
        output_file = input_file.replace('.csv', '_filtered.csv')
    
    filter_by_year_range(input_file, output_file)


if __name__ == '__main__':
    main()
