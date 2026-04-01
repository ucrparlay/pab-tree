#!/usr/bin/env python3
"""
Post-process OSM CSV file to shift longitude and latitude to positive range.
Preserves relative positions by finding min values and shifting all coordinates.
Computes Morton code (Z-order curve) for each point.
"""

import sys
import csv


def interleave_bits(x, y):
    """
    Compute Morton code by interleaving bits of x and y coordinates.
    This creates a Z-order curve index.
    """
    morton = 0
    for i in range(32):  # Process 32 bits for each coordinate
        morton |= ((x & (1 << i)) << i) | ((y & (1 << i)) << (i + 1))
    return morton


def compute_morton_code(lon, lat, lon_scale=1e7, lat_scale=1e7):
    """
    Compute Morton code from longitude and latitude.
    
    Args:
        lon: Longitude (shifted to positive range)
        lat: Latitude (shifted to positive range)
        lon_scale: Scale factor for longitude (default 1e7 for 7 decimal precision)
        lat_scale: Scale factor for latitude (default 1e7 for 7 decimal precision)
    
    Returns:
        Morton code as integer
    """
    # Convert float coordinates to integers by scaling
    x = int(lon * lon_scale)
    y = int(lat * lat_scale)
    
    # Interleave bits to create Morton code
    return interleave_bits(x, y)


def find_min_coordinates(input_file):
    """Find minimum longitude and latitude values."""
    print("Pass 1: Finding minimum coordinates...")
    
    min_lon = float('inf')
    min_lat = float('inf')
    count = 0
    
    with open(input_file, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        
        for row in reader:
            lon = float(row['lon'])
            lat = float(row['lat'])
            
            min_lon = min(min_lon, lon)
            min_lat = min(min_lat, lat)
            
            count += 1
            if count % 1000000 == 0:
                print(f"  Processed {count:,} rows... (current min_lon={min_lon:.7f}, min_lat={min_lat:.7f})")
    
    print(f"\nMinimum coordinates found:")
    print(f"  min_lon: {min_lon:.7f}")
    print(f"  min_lat: {min_lat:.7f}")
    print(f"  Total rows: {count:,}\n")
    
    return min_lon, min_lat, count


def shift_coordinates(input_file, output_file, min_lon, min_lat):
    """Shift all coordinates to positive range and compute Morton codes."""
    print("Pass 2: Shifting coordinates and computing Morton codes...")
    
    # Calculate shift amounts (add small buffer to ensure all values are positive)
    shift_lon = -min_lon if min_lon < 0 else 0
    shift_lat = -min_lat if min_lat < 0 else 0
    
    print(f"Shift amounts:")
    print(f"  lon shift: +{shift_lon:.7f}")
    print(f"  lat shift: +{shift_lat:.7f}")
    print(f"Computing Morton codes (Z-order curve) for spatial indexing...\n")
    
    count = 0
    
    with open(input_file, 'r', encoding='utf-8') as infile, \
         open(output_file, 'w', newline='', encoding='utf-8') as outfile:
        
        reader = csv.DictReader(infile)
        writer = csv.DictWriter(outfile, fieldnames=['id', 'ts', 'lon', 'lat', 'morton'])
        writer.writeheader()
        
        for row in reader:
            # Shift coordinates
            new_lon = float(row['lon']) + shift_lon
            new_lat = float(row['lat']) + shift_lat
            
            # Compute Morton code
            morton_code = compute_morton_code(new_lon, new_lat)
            
            # Write shifted coordinates with Morton code
            writer.writerow({
                'id': row['id'],
                'ts': row['ts'],
                'lon': f"{new_lon:.7f}",
                'lat': f"{new_lat:.7f}",
                'morton': morton_code
            })
            
            count += 1
            if count % 1000000 == 0:
                print(f"  Processed {count:,} rows...")
    
    print(f"\nShift complete! Total rows: {count:,}")
    return count


def main():
    if len(sys.argv) < 2:
        print("Usage: python shift_coordinates.py <input.csv> [output.csv]")
        sys.exit(1)
    
    input_file = sys.argv[1]
    
    # Determine output file name
    if len(sys.argv) >= 3:
        output_file = sys.argv[2]
    else:
        output_file = input_file.replace('.csv', '_shifted.csv')
    
    print(f"Input file: {input_file}")
    print(f"Output file: {output_file}\n")
    
    # Find minimum coordinates
    min_lon, min_lat, total_rows = find_min_coordinates(input_file)
    
    # Shift coordinates
    shifted_rows = shift_coordinates(input_file, output_file, min_lon, min_lat)
    
    print(f"\nPost-processing complete!")
    print(f"Output saved to: {output_file}")


if __name__ == '__main__':
    main()
